/*
 * traced — Trace v2 daemon.
 *
 * Single-threaded event loop. Receives trace events on a unix datagram
 * socket, writes them to SQLite, auto-prunes old data. Serves live
 * event streams and open-span status to trace-viewer over a query socket.
 *
 * Transport:  /run/traced/traced.sock       (SOCK_DGRAM — service events)
 *             UDP port (configurable)       (network events)
 * Query:      /run/traced/traced-query.sock  (SOCK_STREAM — viewer queries)
 * Storage:    /var/lib/traced/trace.db       (WAL mode)
 * Config:     /etc/traced/traced.conf        (prune_hours, prune_interval, udp_port,
 *                                              relay_host, relay_port)
 */

#include <trace/Trace.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <grp.h>
#include <pwd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <sstream>

// =============================================================================
// Configuration
// =============================================================================

static const char* DEFAULT_SOCKET  = "/run/traced/traced.sock";
static const char* DEFAULT_DB_PATH = "/var/lib/traced/trace.db";
static const char* DEFAULT_CONFIG  = "/etc/traced/traced.conf";
static constexpr int DEFAULT_PRUNE_HOURS   = 168;   // 7 days
static constexpr int DEFAULT_PRUNE_INTERVAL = 3600; // check hourly
static constexpr int DEFAULT_UDP_PORT       = 0;      // 0 = disabled
static constexpr int DEFAULT_RELAY_PORT    = 9300;    // upstream traced UDP port
static constexpr int MAX_DGRAM_SIZE        = 65536;
static constexpr int MAX_VIEWERS           = 16;
static constexpr int MAX_CMD_LEN           = 4096;

// =============================================================================
// Config file reader (key=value, # comments, [sections] skipped)
// =============================================================================

static std::string readConfigString(const char* path, const char* key,
                                    const char* default_val) {
    FILE* f = fopen(path, "r");
    if (!f) return default_val;
    char line[512];
    std::string target(key);
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != target) continue;
        std::string v = s.substr(eq + 1);
        size_t start = v.find_first_not_of(" \t");
        fclose(f);
        if (start == std::string::npos) return default_val;
        size_t end = v.find_last_not_of(" \t");
        return v.substr(start, end - start + 1);
    }
    fclose(f);
    return default_val;
}

// =============================================================================
// Data types
// =============================================================================

struct TraceEvent {
    int64_t ts = 0;
    std::string svc;
    std::string cat;
    std::string msg;
    std::string type;        // "span", "emit", "span_start", "span_cancel"
    std::string trace_id;
    std::string span_id;
    std::string fields_json; // raw JSON object string
    int dur_ms = 0;
};

struct OpenSpan {
    std::string span_id;
    std::string svc;
    std::string scope;
    std::string intent;
    std::string trace_id;
    int64_t start_ts;
};

struct ViewerClient {
    int fd = -1;
    enum Mode { TAIL, ERRORS } mode = TAIL;
    std::string service_filter;
    std::string category_filter;
};

// =============================================================================
// Globals
// =============================================================================

static std::atomic<bool> g_running{true};
static sqlite3* g_db = nullptr;
static sqlite3_stmt* g_insert_stmt = nullptr;
static int64_t g_last_prune = 0;
static int g_prune_hours = DEFAULT_PRUNE_HOURS;
static int g_prune_interval = DEFAULT_PRUNE_INTERVAL;
static int64_t g_events_written = 0;

static std::unordered_map<std::string, OpenSpan> g_open_spans;
static std::vector<ViewerClient> g_viewers;

// Relay — forward raw datagrams to upstream traced via UDP (fire-and-forget)
static int g_relay_fd = -1;
static struct sockaddr_in g_relay_addr{};
static int64_t g_events_relayed = 0;

// =============================================================================
// Signal handling
// =============================================================================

static void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

// =============================================================================
// JSON helpers
// =============================================================================

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// =============================================================================
// Minimal JSON parser — for incoming wire format
// =============================================================================

static size_t skipWs(const char* s, size_t pos, size_t len) {
    while (pos < len && (s[pos] == ' ' || s[pos] == '\t' ||
                         s[pos] == '\n' || s[pos] == '\r'))
        pos++;
    return pos;
}

static size_t parseJsonString(const char* s, size_t pos, size_t len,
                              std::string& out) {
    if (pos >= len || s[pos] != '"') return 0;
    pos++;
    out.clear();
    while (pos < len) {
        char c = s[pos++];
        if (c == '"') return pos;
        if (c == '\\' && pos < len) {
            char esc = s[pos++];
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case '/':  out += '/';  break;
                case 'u':  out += "\\u"; break;
                default:   out += esc;  break;
            }
        } else {
            out += c;
        }
    }
    return 0;
}

static size_t parseJsonNumber(const char* s, size_t pos, size_t len,
                              int64_t& out) {
    bool neg = false;
    if (pos < len && s[pos] == '-') { neg = true; pos++; }
    out = 0;
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
        out = out * 10 + (s[pos] - '0');
        pos++;
    }
    if (neg) out = -out;
    return pos;
}

static size_t extractJsonObject(const char* s, size_t pos, size_t len,
                                std::string& out) {
    if (pos >= len || s[pos] != '{') return 0;
    int depth = 0;
    size_t start = pos;
    bool in_string = false;
    while (pos < len) {
        char c = s[pos];
        if (in_string) {
            if (c == '\\') { pos++; }
            else if (c == '"') { in_string = false; }
        } else {
            if (c == '"') { in_string = true; }
            else if (c == '{') { depth++; }
            else if (c == '}') {
                depth--;
                if (depth == 0) {
                    pos++;
                    out.assign(s + start, pos - start);
                    return pos;
                }
            }
        }
        pos++;
    }
    return 0;
}

static bool parseEvent(const char* data, size_t len, TraceEvent& ev) {
    size_t pos = skipWs(data, 0, len);
    if (pos >= len || data[pos] != '{') return false;
    pos++;

    while (pos < len) {
        pos = skipWs(data, pos, len);
        if (pos >= len) return false;
        if (data[pos] == '}') return true;
        if (data[pos] == ',') { pos++; continue; }

        std::string key;
        pos = parseJsonString(data, pos, len, key);
        if (pos == 0) return false;

        pos = skipWs(data, pos, len);
        if (pos >= len || data[pos] != ':') return false;
        pos++;
        pos = skipWs(data, pos, len);
        if (pos >= len) return false;

        if (data[pos] == '"') {
            std::string val;
            pos = parseJsonString(data, pos, len, val);
            if (pos == 0) return false;
            if (key == "svc")           ev.svc = std::move(val);
            else if (key == "cat")      ev.cat = std::move(val);
            else if (key == "msg")      ev.msg = std::move(val);
            else if (key == "type")     ev.type = std::move(val);
            else if (key == "trace_id") ev.trace_id = std::move(val);
            else if (key == "span_id")  ev.span_id = std::move(val);
        } else if (data[pos] == '{') {
            std::string obj;
            pos = extractJsonObject(data, pos, len, obj);
            if (pos == 0) return false;
            if (key == "fields") ev.fields_json = std::move(obj);
        } else {
            int64_t num = 0;
            pos = parseJsonNumber(data, pos, len, num);
            if (key == "ts")      ev.ts = num;
            else if (key == "dur_ms") ev.dur_ms = static_cast<int>(num);
        }
    }
    return true;
}

// =============================================================================
// Query options parser
// =============================================================================

struct QueryOptions {
    int since = 300;
    int limit = 200;
    std::string service;
    std::string category;
};

static void parseQueryOptions(const char* json, size_t len, QueryOptions& opts) {
    size_t pos = skipWs(json, 0, len);
    if (pos >= len || json[pos] != '{') return;
    pos++;

    while (pos < len) {
        pos = skipWs(json, pos, len);
        if (pos >= len || json[pos] == '}') return;
        if (json[pos] == ',') { pos++; continue; }

        std::string key;
        pos = parseJsonString(json, pos, len, key);
        if (pos == 0) return;

        pos = skipWs(json, pos, len);
        if (pos >= len || json[pos] != ':') return;
        pos++;
        pos = skipWs(json, pos, len);

        if (json[pos] == '"') {
            std::string val;
            pos = parseJsonString(json, pos, len, val);
            if (pos == 0) return;
            if (key == "service")       opts.service = std::move(val);
            else if (key == "category") opts.category = std::move(val);
        } else {
            int64_t num = 0;
            pos = parseJsonNumber(json, pos, len, num);
            if (key == "since")      opts.since = static_cast<int>(num);
            else if (key == "limit") opts.limit = static_cast<int>(num);
        }
    }
}

// =============================================================================
// Database
// =============================================================================

static bool openDatabase(const char* path) {
    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[traced] Failed to open %s: %s\n",
                path, sqlite3_errmsg(g_db));
        return false;
    }

    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);

    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS trace_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            service TEXT NOT NULL,
            category TEXT NOT NULL,
            message TEXT,
            fields TEXT,
            duration_ms INTEGER DEFAULT 0,
            trace_id TEXT DEFAULT '',
            type TEXT DEFAULT 'emit'
        );
        CREATE INDEX IF NOT EXISTS idx_trace_ts  ON trace_events(timestamp);
        CREATE INDEX IF NOT EXISTS idx_trace_svc ON trace_events(service);
        CREATE INDEX IF NOT EXISTS idx_trace_cat ON trace_events(category);
        CREATE INDEX IF NOT EXISTS idx_trace_tid ON trace_events(trace_id);
    )";

    char* err = nullptr;
    sqlite3_exec(g_db, schema, nullptr, nullptr, &err);
    if (err) {
        fprintf(stderr, "[traced] Schema init failed: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    // Migrate v1 databases: add type column if missing
    sqlite3_exec(g_db,
        "ALTER TABLE trace_events ADD COLUMN type TEXT DEFAULT 'emit'",
        nullptr, nullptr, nullptr);

    const char* sql = R"(
        INSERT INTO trace_events
            (timestamp, service, category, message, fields, duration_ms, trace_id, type)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )";
    rc = sqlite3_prepare_v2(g_db, sql, -1, &g_insert_stmt, nullptr);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[traced] Failed to prepare insert: %s\n",
                sqlite3_errmsg(g_db));
        return false;
    }

    return true;
}

static void writeEvent(const TraceEvent& ev) {
    if (!g_insert_stmt) return;

    sqlite3_reset(g_insert_stmt);
    sqlite3_bind_int64(g_insert_stmt, 1, ev.ts);
    sqlite3_bind_text(g_insert_stmt, 2, ev.svc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert_stmt, 3, ev.cat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert_stmt, 4, ev.msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert_stmt, 5, ev.fields_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(g_insert_stmt, 6, ev.dur_ms);
    sqlite3_bind_text(g_insert_stmt, 7, ev.trace_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(g_insert_stmt, 8, ev.type.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(g_insert_stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[traced] Insert failed: %s\n", sqlite3_errmsg(g_db));
    } else {
        g_events_written++;
    }
}

static void pruneOldEvents() {
    if (!g_db) return;

    trace::Span span("prune", "periodic prune");
    span.set("retention_hours", std::to_string(g_prune_hours).c_str());

    int64_t cutoff = std::time(nullptr) - (g_prune_hours * 3600);

    char* err = nullptr;
    char sql[128];
    std::snprintf(sql, sizeof(sql),
        "DELETE FROM trace_events WHERE timestamp < %ld", (long)cutoff);
    sqlite3_exec(g_db, sql, nullptr, nullptr, &err);
    if (err) {
        fprintf(stderr, "[traced] Prune failed: %s\n", err);
        span.markError();
        span.set("error_detail", err);
        sqlite3_free(err);
    } else {
        int deleted = sqlite3_changes(g_db);
        span.set("rows_deleted", std::to_string(deleted).c_str());
    }

    g_last_prune = std::time(nullptr);
}

static void closeDatabase() {
    if (g_insert_stmt) {
        sqlite3_finalize(g_insert_stmt);
        g_insert_stmt = nullptr;
    }
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

// =============================================================================
// Sockets
// =============================================================================

static int createRecvSocket(const char* path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[traced] recv socket()"); return -1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[traced] recv bind()");
        close(fd);
        return -1;
    }

    chmod(path, 0660);
    struct group* grp = getgrnam("traced");
    if (grp) chown(path, static_cast<uid_t>(-1), grp->gr_gid);
    return fd;
}

static int createQuerySocket(const char* path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("[traced] query socket()"); return -1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[traced] query bind()");
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        perror("[traced] listen()");
        close(fd);
        return -1;
    }

    chmod(path, 0660);
    struct group* grp = getgrnam("traced");
    if (grp) chown(path, static_cast<uid_t>(-1), grp->gr_gid);
    return fd;
}

static int createUdpSocket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[traced] udp socket()"); return -1; }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[traced] udp bind()");
        close(fd);
        return -1;
    }

    return fd;
}

static std::string deriveQueryPath(const std::string& base) {
    auto dot = base.rfind('.');
    if (dot != std::string::npos)
        return base.substr(0, dot) + "-query" + base.substr(dot);
    return base + "-query";
}

// =============================================================================
// Viewer — backlog & status
// =============================================================================

static bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static void sendBacklog(int fd, const QueryOptions& opts, bool errors_only) {
    if (!g_db) return;

    std::string sql = "SELECT id, timestamp, service, category, message, "
                      "fields, duration_ms, trace_id, type "
                      "FROM trace_events WHERE timestamp >= ?";

    std::vector<std::string> bind_strs;

    if (errors_only)
        sql += " AND (category = 'error' OR fields LIKE '%\"error\"%')";
    if (!opts.service.empty()) {
        sql += " AND service = ?";
        bind_strs.push_back(opts.service);
    }
    if (!opts.category.empty() && !errors_only) {
        sql += " AND category = ?";
        bind_strs.push_back(opts.category);
    }

    sql += " ORDER BY id DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return;

    int64_t since_ts = std::time(nullptr) - opts.since;
    int bi = 1;
    sqlite3_bind_int64(stmt, bi++, since_ts);
    for (const auto& s : bind_strs)
        sqlite3_bind_text(stmt, bi++, s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, bi, opts.limit);

    auto colText = [&](int col) -> std::string {
        auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return s ? s : "";
    };

    // Collect in reverse (we ORDER DESC, need to send ASC)
    std::vector<std::string> lines;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::ostringstream json;
        json << "{\"id\":" << sqlite3_column_int64(stmt, 0)
             << ",\"ts\":" << sqlite3_column_int64(stmt, 1)
             << ",\"svc\":\"" << jsonEscape(colText(2)) << "\""
             << ",\"cat\":\"" << jsonEscape(colText(3)) << "\""
             << ",\"msg\":\"" << jsonEscape(colText(4)) << "\"";

        std::string fields = colText(5);
        if (!fields.empty()) {
            if (fields[0] == '{')
                json << ",\"fields\":" << fields;
            else
                json << ",\"fields\":\"" << jsonEscape(fields) << "\"";
        }

        json << ",\"dur_ms\":" << sqlite3_column_int(stmt, 6)
             << ",\"trace_id\":\"" << jsonEscape(colText(7)) << "\""
             << ",\"type\":\"" << jsonEscape(colText(8)) << "\""
             << "}\n";
        lines.push_back(json.str());
    }
    sqlite3_finalize(stmt);

    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!sendAll(fd, it->c_str(), it->size())) return;
    }
}

static void sendStatus(int fd) {
    std::ostringstream json;
    json << "{\"spans\":[";
    bool first = true;
    int64_t now = std::time(nullptr);
    for (const auto& [id, span] : g_open_spans) {
        if (!first) json << ",";
        json << "{\"span_id\":\"" << jsonEscape(span.span_id) << "\""
             << ",\"svc\":\"" << jsonEscape(span.svc) << "\""
             << ",\"cat\":\"" << jsonEscape(span.scope) << "\""
             << ",\"msg\":\"" << jsonEscape(span.intent) << "\""
             << ",\"ts\":" << span.start_ts
             << ",\"elapsed_s\":" << (now - span.start_ts);
        if (!span.trace_id.empty())
            json << ",\"trace_id\":\"" << jsonEscape(span.trace_id) << "\"";
        json << "}";
        first = false;
    }
    json << "]}\n";
    std::string s = json.str();
    sendAll(fd, s.c_str(), s.size());
}

// =============================================================================
// Viewer — connection handling
// =============================================================================

static void handleNewViewer(int listen_fd) {
    int client_fd = accept(listen_fd, nullptr, nullptr);
    if (client_fd < 0) return;

    if (static_cast<int>(g_viewers.size()) >= MAX_VIEWERS) {
        close(client_fd);
        return;
    }

    // Read command with 1s timeout
    struct pollfd pfd{client_fd, POLLIN, 0};
    if (poll(&pfd, 1, 1000) <= 0) {
        close(client_fd);
        return;
    }

    char buf[MAX_CMD_LEN];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    // Strip trailing newline/whitespace
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';

    // Split command and JSON options
    std::string command;
    QueryOptions opts;
    char* space = strchr(buf, ' ');
    if (space) {
        command.assign(buf, space - buf);
        size_t json_len = strlen(space + 1);
        parseQueryOptions(space + 1, json_len, opts);
    } else {
        command = buf;
    }

    if (command == "STATUS") {
        sendStatus(client_fd);
        close(client_fd);
        return;
    }

    bool errors_only = (command == "ERRORS");

    // Send backlog from DB
    sendBacklog(client_fd, opts, errors_only);

    // Send end-of-backlog marker
    static const char marker[] = "{\"__marker\":\"end_backlog\"}\n";
    if (!sendAll(client_fd, marker, sizeof(marker) - 1)) {
        close(client_fd);
        return;
    }

    // Add to live viewer list
    ViewerClient vc;
    vc.fd = client_fd;
    vc.mode = errors_only ? ViewerClient::ERRORS : ViewerClient::TAIL;
    vc.service_filter = opts.service;
    vc.category_filter = opts.category;
    g_viewers.push_back(vc);

    trace::emit("viewer", "connected", {
        {"mode", errors_only ? "errors" : "tail"},
        {"viewers", std::to_string(g_viewers.size()).c_str()}});
}

// =============================================================================
// Event forwarding to viewers
// =============================================================================

static void forwardToViewers(const TraceEvent& ev) {
    if (g_viewers.empty()) return;

    // Build JSON line
    std::ostringstream json;
    json << "{\"ts\":" << ev.ts
         << ",\"svc\":\"" << jsonEscape(ev.svc) << "\""
         << ",\"cat\":\"" << jsonEscape(ev.cat) << "\""
         << ",\"msg\":\"" << jsonEscape(ev.msg) << "\"";
    if (ev.dur_ms > 0)
        json << ",\"dur_ms\":" << ev.dur_ms;
    if (!ev.trace_id.empty())
        json << ",\"trace_id\":\"" << jsonEscape(ev.trace_id) << "\"";
    json << ",\"type\":\"" << jsonEscape(ev.type) << "\"";
    if (!ev.fields_json.empty())
        json << ",\"fields\":" << ev.fields_json;
    json << "}\n";
    std::string line = json.str();

    bool is_error = (ev.cat == "error" ||
                     ev.fields_json.find("\"error\"") != std::string::npos);

    for (auto& v : g_viewers) {
        if (v.fd < 0) continue;
        if (v.mode == ViewerClient::ERRORS && !is_error) continue;
        if (!v.service_filter.empty() && ev.svc != v.service_filter) continue;
        if (!v.category_filter.empty() && ev.cat != v.category_filter) continue;

        ssize_t n = send(v.fd, line.c_str(), line.size(),
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN) {
            close(v.fd);
            v.fd = -1;
        }
    }

    // Remove disconnected viewers
    g_viewers.erase(
        std::remove_if(g_viewers.begin(), g_viewers.end(),
                        [](const ViewerClient& v) { return v.fd < 0; }),
        g_viewers.end());
}

// =============================================================================
// Relay — fire-and-forget UDP forward to upstream traced
// =============================================================================

static void relayEvent(const char* data, size_t len) {
    if (g_relay_fd < 0) return;
    ssize_t n = sendto(g_relay_fd, data, len, MSG_DONTWAIT,
                       reinterpret_cast<const struct sockaddr*>(&g_relay_addr),
                       sizeof(g_relay_addr));
    if (n > 0) g_events_relayed++;
}

// =============================================================================
// Event processing
// =============================================================================

static void processEvent(const char* data, size_t len) {
    TraceEvent ev;
    if (!parseEvent(data, len, ev)) {
        fprintf(stderr, "[traced] Parse error: %.80s...\n", data);
        return;
    }

    // Relay raw datagram to upstream traced (fire-and-forget, before local processing)
    relayEvent(data, len);

    if (ev.ts == 0) ev.ts = std::time(nullptr);
    if (ev.type.empty()) ev.type = "emit";

    // Span lifecycle tracking (open spans — NOT written to DB)
    if (ev.type == "span_start") {
        OpenSpan os;
        os.span_id = ev.span_id;
        os.svc = ev.svc;
        os.scope = ev.cat;
        os.intent = ev.msg;
        os.start_ts = ev.ts;
        os.trace_id = ev.trace_id;
        g_open_spans[ev.span_id] = std::move(os);
        return;
    }

    if (ev.type == "span_cancel") {
        g_open_spans.erase(ev.span_id);
        return;
    }

    // Completed span — remove from open tracking
    if (ev.type == "span" && !ev.span_id.empty()) {
        g_open_spans.erase(ev.span_id);
    }

    // Write to DB
    writeEvent(ev);

    // Forward to connected viewers
    forwardToViewers(ev);
}

// =============================================================================
// Main
// =============================================================================

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [-s socket_path] [-d db_path] [-c config_path] [-p prune_hours] [-u udp_port] [-r host:port]\n", prog);
    fprintf(stderr, "  -s  Unix datagram socket (default: %s)\n", DEFAULT_SOCKET);
    fprintf(stderr, "  -d  SQLite database path (default: %s)\n", DEFAULT_DB_PATH);
    fprintf(stderr, "  -c  Config file path (default: %s)\n", DEFAULT_CONFIG);
    fprintf(stderr, "  -p  Prune events older than N hours (default: %d)\n", DEFAULT_PRUNE_HOURS);
    fprintf(stderr, "  -u  UDP port for network events (default: %d = disabled)\n", DEFAULT_UDP_PORT);
    fprintf(stderr, "  -r  Relay events to upstream traced (host:port, default: disabled)\n");
    fprintf(stderr, "\nConfig keys: prune_hours, prune_interval, udp_port, relay_host, relay_port\n");
    fprintf(stderr, "CLI flags override config file values.\n");
    fprintf(stderr, "Query socket is derived: <socket>-query (e.g. traced-query.sock)\n");
}

int main(int argc, char* argv[]) {
    const char* socket_path = DEFAULT_SOCKET;
    const char* db_path = DEFAULT_DB_PATH;
    const char* config_path = DEFAULT_CONFIG;
    int udp_port = DEFAULT_UDP_PORT;
    std::string relay_host;
    int relay_port = DEFAULT_RELAY_PORT;

    // First pass: pick up -c before reading config
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) { config_path = argv[i + 1]; break; }
    }

    // Config file defaults (CLI flags override)
    std::string cfg_val;
    cfg_val = readConfigString(config_path, "prune_hours", "");
    if (!cfg_val.empty()) g_prune_hours = atoi(cfg_val.c_str());
    cfg_val = readConfigString(config_path, "prune_interval", "");
    if (!cfg_val.empty()) g_prune_interval = atoi(cfg_val.c_str());
    cfg_val = readConfigString(config_path, "udp_port", "");
    if (!cfg_val.empty()) udp_port = atoi(cfg_val.c_str());
    cfg_val = readConfigString(config_path, "relay_host", "");
    if (!cfg_val.empty()) relay_host = cfg_val;
    cfg_val = readConfigString(config_path, "relay_port", "");
    if (!cfg_val.empty()) relay_port = atoi(cfg_val.c_str());

    int opt;
    while ((opt = getopt(argc, argv, "s:d:c:p:u:r:h")) != -1) {
        switch (opt) {
            case 's': socket_path = optarg; break;
            case 'd': db_path = optarg; break;
            case 'c': break; // already handled above
            case 'p': g_prune_hours = atoi(optarg); break;
            case 'u': udp_port = atoi(optarg); break;
            case 'r': {
                // Parse host:port
                std::string arg(optarg);
                auto colon = arg.rfind(':');
                if (colon != std::string::npos) {
                    relay_host = arg.substr(0, colon);
                    relay_port = atoi(arg.substr(colon + 1).c_str());
                } else {
                    relay_host = arg;
                }
                break;
            }
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    std::string query_path = deriveQueryPath(socket_path);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[traced] Starting — recv=%s query=%s db=%s prune=%dh interval=%ds udp=%d relay=%s\n",
            socket_path, query_path.c_str(), db_path, g_prune_hours, g_prune_interval, udp_port,
            relay_host.empty() ? "none" : (relay_host + ":" + std::to_string(relay_port)).c_str());

    if (!openDatabase(db_path)) return 1;

    int recv_fd = createRecvSocket(socket_path);
    if (recv_fd < 0) {
        closeDatabase();
        return 1;
    }

    int query_fd = createQuerySocket(query_path.c_str());
    if (query_fd < 0) {
        fprintf(stderr, "[traced] Warning: query socket failed — viewer disabled\n");
    }

    int udp_fd = -1;
    if (udp_port > 0) {
        udp_fd = createUdpSocket(udp_port);
        if (udp_fd < 0) {
            fprintf(stderr, "[traced] Warning: UDP socket failed — network trace disabled\n");
        } else {
            fprintf(stderr, "[traced] UDP listener on port %d\n", udp_port);
        }
    }

    // Relay — outbound UDP to upstream traced
    if (!relay_host.empty()) {
        g_relay_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_relay_fd < 0) {
            fprintf(stderr, "[traced] Warning: relay socket failed — relay disabled\n");
        } else {
            g_relay_addr.sin_family = AF_INET;
            g_relay_addr.sin_port = htons(static_cast<uint16_t>(relay_port));
            if (inet_pton(AF_INET, relay_host.c_str(), &g_relay_addr.sin_addr) != 1) {
                fprintf(stderr, "[traced] Warning: invalid relay_host '%s' — relay disabled\n",
                        relay_host.c_str());
                close(g_relay_fd);
                g_relay_fd = -1;
            } else {
                fprintf(stderr, "[traced] Relay to %s:%d\n", relay_host.c_str(), relay_port);
            }
        }
    }

    // Self-trace — connect to our own recv socket
    trace::init("traced", socket_path);

    pruneOldEvents();

    trace::emit("lifecycle", "started", {
        {"prune_hours", std::to_string(g_prune_hours).c_str()},
        {"prune_interval", std::to_string(g_prune_interval).c_str()},
        {"udp_port", std::to_string(udp_port).c_str()},
        {"relay", relay_host.empty() ? "none" : (relay_host + ":" + std::to_string(relay_port)).c_str()},
        {"db", db_path}});

    fprintf(stderr, "[traced] Ready — listening\n");

    char buf[MAX_DGRAM_SIZE];

    while (g_running.load(std::memory_order_acquire)) {
        // Build poll set: [recv, udp?, query?, viewer0, viewer1, ...]
        std::vector<struct pollfd> pfds;
        pfds.push_back({recv_fd, POLLIN, 0});           // index 0: unix dgram
        int udp_idx = -1;
        if (udp_fd >= 0) {
            udp_idx = static_cast<int>(pfds.size());
            pfds.push_back({udp_fd, POLLIN, 0});
        }
        int query_idx = -1;
        if (query_fd >= 0) {
            query_idx = static_cast<int>(pfds.size());
            pfds.push_back({query_fd, POLLIN, 0});
        }
        size_t viewer_base = pfds.size();
        for (const auto& v : g_viewers)
            pfds.push_back({v.fd, POLLIN, 0});

        int ret = poll(pfds.data(), pfds.size(), 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[traced] poll()");
            break;
        }

        // Unix datagram event
        if (pfds[0].revents & POLLIN) {
            ssize_t n = recv(recv_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                processEvent(buf, static_cast<size_t>(n));
            }
        }

        // UDP network event
        if (udp_idx >= 0 && pfds[static_cast<size_t>(udp_idx)].revents & POLLIN) {
            ssize_t n = recv(udp_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                processEvent(buf, static_cast<size_t>(n));
            }
        }

        // New viewer connection
        if (query_idx >= 0 && pfds[static_cast<size_t>(query_idx)].revents & POLLIN) {
            handleNewViewer(query_fd);
        }

        // Check viewer clients for disconnect
        for (size_t i = viewer_base; i < pfds.size(); i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                size_t vi = i - viewer_base;
                if (vi < g_viewers.size()) {
                    char dummy[64];
                    ssize_t n = recv(g_viewers[vi].fd, dummy, sizeof(dummy),
                                     MSG_DONTWAIT);
                    if (n <= 0) {
                        close(g_viewers[vi].fd);
                        g_viewers[vi].fd = -1;
                    }
                }
            }
        }

        // Remove disconnected viewers
        size_t before = g_viewers.size();
        g_viewers.erase(
            std::remove_if(g_viewers.begin(), g_viewers.end(),
                            [](const ViewerClient& v) { return v.fd < 0; }),
            g_viewers.end());
        if (g_viewers.size() < before) {
            trace::emit("viewer", "disconnected", {
                {"viewers", std::to_string(g_viewers.size()).c_str()}});
        }

        // Periodic prune
        int64_t now = std::time(nullptr);
        if (now - g_last_prune >= g_prune_interval) {
            pruneOldEvents();
        }
    }

    trace::emit("lifecycle", "shutdown", {
        {"events_written", std::to_string(g_events_written).c_str()},
        {"events_relayed", std::to_string(g_events_relayed).c_str()},
        {"open_spans", std::to_string(g_open_spans.size()).c_str()}});

    fprintf(stderr, "[traced] Shutting down — %ld events written, %ld relayed, %zu open spans\n",
            (long)g_events_written, (long)g_events_relayed, g_open_spans.size());

    trace::shutdown();

    close(recv_fd);
    unlink(socket_path);

    if (udp_fd >= 0) close(udp_fd);
    if (g_relay_fd >= 0) close(g_relay_fd);

    for (auto& v : g_viewers) {
        if (v.fd >= 0) close(v.fd);
    }
    if (query_fd >= 0) {
        close(query_fd);
        unlink(query_path.c_str());
    }

    closeDatabase();
    return 0;
}
