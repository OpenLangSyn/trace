/*
 * test_trace — Trace v2 integration test harness.
 *
 * Starts a traced daemon with a test socket + temp DB, exercises the
 * client library (init, Span, emit, log), then queries the DB to
 * verify events landed correctly.
 *
 * Build: make test
 * Run:   ./test_trace
 */

#include <trace/Trace.h>
#include <sqlite3.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

static const char* TEST_SOCKET       = "/tmp/traced_test.sock";
static const char* TEST_QUERY_SOCKET = "/tmp/traced_test-query.sock";
static const char* TEST_DB           = "/tmp/traced_test.db";
static const char* TRACED_BIN        = "./traced";

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failed++; \
    } else { \
        fprintf(stderr, "  PASS: %s\n", msg); \
        g_passed++; \
    } \
} while (0)

// ═══════════════════════════════════════════════════════════════════════════════
// DB query helpers
// ═══════════════════════════════════════════════════════════════════════════════

struct Row {
    int64_t timestamp;
    std::string service;
    std::string category;
    std::string message;
    std::string fields;
    int duration_ms;
    std::string trace_id;
    std::string type;
};

static std::vector<Row> queryAll(const char* db_path) {
    std::vector<Row> rows;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return rows;

    const char* sql = R"(
        SELECT timestamp, service, category, message, fields,
               duration_ms, trace_id, COALESCE(type, 'emit')
        FROM trace_events ORDER BY id ASC
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return rows;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row r;
        r.timestamp = sqlite3_column_int64(stmt, 0);
        auto text = [&](int col) -> std::string {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return s ? s : "";
        };
        r.service = text(1);
        r.category = text(2);
        r.message = text(3);
        r.fields = text(4);
        r.duration_ms = sqlite3_column_int(stmt, 5);
        r.trace_id = text(6);
        r.type = text(7);
        rows.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test runner
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    fprintf(stderr, "=== Trace v2 Integration Test ===\n\n");

    // Clean up any previous test artifacts
    unlink(TEST_SOCKET);
    unlink(TEST_QUERY_SOCKET);
    unlink(TEST_DB);

    // Start traced daemon in background
    fprintf(stderr, "Starting traced daemon...\n");
    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        execl(TRACED_BIN, TRACED_BIN, "-s", TEST_SOCKET, "-d", TEST_DB, nullptr);
        perror("execl traced");
        _exit(1);
    }
    if (daemon_pid < 0) {
        perror("fork");
        return 1;
    }

    // Wait for socket to appear
    for (int i = 0; i < 50; i++) {
        if (access(TEST_SOCKET, F_OK) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (access(TEST_SOCKET, F_OK) != 0) {
        fprintf(stderr, "FATAL: traced socket did not appear\n");
        kill(daemon_pid, SIGTERM);
        waitpid(daemon_pid, nullptr, 0);
        return 1;
    }

    fprintf(stderr, "Daemon ready (pid %d)\n\n", daemon_pid);

    // ── Test 1: init ────────────────────────────────────────────────────────
    fprintf(stderr, "Test 1: init/connected\n");
    bool ok = trace::init("test-harness", TEST_SOCKET);
    ASSERT(ok, "init() returns true");
    ASSERT(trace::connected(), "connected() returns true after init");

    // ── Test 2: emit ────────────────────────────────────────────────────────
    fprintf(stderr, "\nTest 2: emit\n");
    trace::emit("cache", "hit — lookup bypassed", {{"key", "abc123"}});

    // ── Test 3: Span ────────────────────────────────────────────────────────
    fprintf(stderr, "\nTest 3: Span\n");
    {
        trace::Span span("query", "BFS lookup — entity by name");
        span.set("entity", "Merlin");
        span.set("depth", "3");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ── Test 4: Span with trace_id and error ────────────────────────────────
    fprintf(stderr, "\nTest 4: Span with trace_id + error\n");
    std::string tid = trace::generateTraceId();
    {
        trace::Span span("pipeline", "decompose query");
        span.setTraceId(tid.c_str());
        span.markError();
        span.set("reason", "timeout");
    }

    // ── Test 5: Span with cancel ────────────────────────────────────────────
    fprintf(stderr, "\nTest 5: Span cancel\n");
    {
        trace::Span span("route", "should not appear");
        span.cancel();
    }

    // ── Test 6: log ─────────────────────────────────────────────────────────
    fprintf(stderr, "\nTest 6: log\n");
    trace::log(trace::INFO, "started on port %d", 8891);
    trace::log(trace::ERROR, "connection lost to %s", "upstream");

    // ── Test 7: multiple rapid emits ────────────────────────────────────────
    fprintf(stderr, "\nTest 7: burst emit (20 events)\n");
    for (int i = 0; i < 20; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "burst event %d", i);
        trace::emit("burst", msg);
    }

    // Give daemon time to flush all events to SQLite
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Shutdown client
    trace::shutdown();
    ASSERT(!trace::connected(), "connected() returns false after shutdown");

    // Stop daemon
    kill(daemon_pid, SIGTERM);
    int status;
    waitpid(daemon_pid, &status, 0);
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "daemon exited cleanly");

    // ── Verify database ─────────────────────────────────────────────────────
    fprintf(stderr, "\nVerifying database...\n");
    auto rows = queryAll(TEST_DB);
    fprintf(stderr, "  Total events in DB: %zu\n", rows.size());

    // Expected: 1 emit + 1 span + 1 error span + 2 logs + 20 burst = 25
    // Plus daemon self-trace (svc="claude"): 1 lifecycle/started + 1 prune span = 27
    // (cancelled span should NOT appear)
    ASSERT(rows.size() == 27, "expected 27 events total (25 test + 2 self-trace)");

    // Check emit event
    bool found_emit = false;
    for (const auto& r : rows) {
        if (r.category == "cache" && r.message == "hit — lookup bypassed") {
            found_emit = true;
            ASSERT(r.service == "test-harness", "emit: correct service");
            ASSERT(r.type == "emit", "emit: type is 'emit'");
            ASSERT(r.fields.find("abc123") != std::string::npos,
                   "emit: fields contain key value");
        }
    }
    ASSERT(found_emit, "emit event found in DB");

    // Check span event
    bool found_span = false;
    for (const auto& r : rows) {
        if (r.category == "query" && r.type == "span") {
            found_span = true;
            ASSERT(r.service == "test-harness", "span: correct service");
            ASSERT(r.duration_ms >= 40, "span: duration >= 40ms");
            ASSERT(r.fields.find("Merlin") != std::string::npos,
                   "span: fields contain entity");
        }
    }
    ASSERT(found_span, "span event found in DB");

    // Check error span
    bool found_error = false;
    for (const auto& r : rows) {
        if (r.category == "pipeline" && r.type == "span") {
            found_error = true;
            ASSERT(!r.trace_id.empty(), "error span: has trace_id");
            ASSERT(r.trace_id == tid, "error span: trace_id matches");
            ASSERT(r.fields.find("error") != std::string::npos,
                   "error span: fields contain error");
        }
    }
    ASSERT(found_error, "error span found in DB");

    // Check cancelled span is NOT in DB
    bool found_cancelled = false;
    for (const auto& r : rows) {
        if (r.message == "should not appear") found_cancelled = true;
    }
    ASSERT(!found_cancelled, "cancelled span not in DB");

    // Check log events
    int log_count = 0;
    for (const auto& r : rows) {
        if (r.category == "log") log_count++;
    }
    ASSERT(log_count == 2, "2 log events found");

    // Check burst events
    int burst_count = 0;
    for (const auto& r : rows) {
        if (r.category == "burst") burst_count++;
    }
    ASSERT(burst_count == 20, "20 burst events found");

    // ── Summary ─────────────────────────────────────────────────────────────
    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n",
            g_passed, g_failed);

    // Clean up
    unlink(TEST_SOCKET);
    unlink(TEST_QUERY_SOCKET);
    unlink(TEST_DB);

    return g_failed > 0 ? 1 : 0;
}
