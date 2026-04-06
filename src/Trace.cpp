/*
 * Trace.cpp — Trace v2 client library.
 *
 * Sends structured trace events to a collector daemon over a
 * unix datagram socket. Services never touch SQLite.
 *
 * Copyright (c) 2025-2026 Are Bjørby <are.bjorby@langsyn.org>
 * SPDX-License-Identifier: MIT
 */

#include <trace/Trace.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <mutex>
#include <atomic>

namespace trace {

// ═══════════════════════════════════════════════════════════════════════════════
// Internal state
// ═══════════════════════════════════════════════════════════════════════════════

static int s_fd = -1;
static struct sockaddr_un s_dest;
static std::string s_service;
static std::mutex s_mutex;
static std::atomic<bool> s_connected{false};

// ═══════════════════════════════════════════════════════════════════════════════
// Wire format — one JSON object per datagram
// ═══════════════════════════════════════════════════════════════════════════════

static std::string escapeJson(const std::string& s) {
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

static void sendDatagram(const std::string& json) {
    if (!s_connected.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_fd < 0) return;
    // Fire-and-forget. Silently drops if daemon is down or buffer full.
    sendto(s_fd, json.c_str(), json.size(), MSG_DONTWAIT,
           reinterpret_cast<const struct sockaddr*>(&s_dest), sizeof(s_dest));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Connection
// ═══════════════════════════════════════════════════════════════════════════════

bool init(const std::string& service_name, const std::string& socket_path) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_fd >= 0) return true;  // already initialized

    s_service = service_name;

    s_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s_fd < 0) {
        fprintf(stderr, "[trace] socket() failed: %s\n", strerror(errno));
        return false;
    }

    // Bind client to an autobind address (empty path → kernel assigns)
    struct sockaddr_un client{};
    client.sun_family = AF_UNIX;
    if (bind(s_fd, reinterpret_cast<struct sockaddr*>(&client),
             sizeof(sa_family_t)) < 0) {
        fprintf(stderr, "[trace] bind() failed: %s\n", strerror(errno));
        close(s_fd);
        s_fd = -1;
        return false;
    }

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sun_family = AF_UNIX;
    strncpy(s_dest.sun_path, socket_path.c_str(), sizeof(s_dest.sun_path) - 1);

    s_connected.store(true, std::memory_order_release);
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_connected.store(false, std::memory_order_release);
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

bool connected() {
    return s_connected.load(std::memory_order_acquire);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Span
// ═══════════════════════════════════════════════════════════════════════════════

static std::atomic<uint64_t> s_spanSeq{0};

static std::string generateSpanId() {
    auto seq = s_spanSeq.fetch_add(1, std::memory_order_relaxed);
    auto pid = static_cast<uint16_t>(getpid());
    char buf[20];
    std::snprintf(buf, sizeof(buf), "s%04x%08lx", pid, (unsigned long)(seq & 0xFFFFFFFF));
    return buf;
}

Span::Span(const char* scope, const char* intent)
    : scope_(scope), intent_(intent),
      span_id_(generateSpanId()),
      start_(std::chrono::steady_clock::now())
{
    int64_t ts = std::time(nullptr);
    std::ostringstream json;
    json << "{\"ts\":" << ts
         << ",\"svc\":\"" << escapeJson(s_service) << "\""
         << ",\"cat\":\"" << escapeJson(scope_) << "\""
         << ",\"msg\":\"" << escapeJson(intent_) << "\""
         << ",\"type\":\"span_start\""
         << ",\"span_id\":\"" << span_id_ << "\""
         << "}";
    sendDatagram(json.str());
}

Span::~Span() {
    if (cancelled_) {
        // Emit span_cancel so daemon removes from open span tracking
        std::ostringstream json;
        json << "{\"type\":\"span_cancel\""
             << ",\"span_id\":\"" << span_id_ << "\""
             << "}";
        sendDatagram(json.str());
        return;
    }

    auto elapsed = std::chrono::steady_clock::now() - start_;
    int dur_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

    int64_t ts = std::time(nullptr);

    std::ostringstream json;
    json << "{\"ts\":" << ts
         << ",\"svc\":\"" << escapeJson(s_service) << "\""
         << ",\"cat\":\"" << escapeJson(scope_) << "\""
         << ",\"msg\":\"" << escapeJson(intent_) << "\""
         << ",\"dur_ms\":" << dur_ms
         << ",\"type\":\"span\""
         << ",\"span_id\":\"" << span_id_ << "\"";

    if (!trace_id_.empty())
        json << ",\"trace_id\":\"" << escapeJson(trace_id_) << "\"";

    if (error_)
        fields_["error"] = "true";

    if (!fields_.empty()) {
        json << ",\"fields\":{";
        bool first = true;
        for (const auto& [k, v] : fields_) {
            if (!first) json << ",";
            json << "\"" << escapeJson(k) << "\":\"" << escapeJson(v) << "\"";
            first = false;
        }
        json << "}";
    }

    json << "}";
    sendDatagram(json.str());
}

void Span::set(const char* key, const char* value) {
    fields_[key] = value;
}

void Span::setTraceId(const char* id) {
    trace_id_ = id;
}

void Span::markError() {
    error_ = true;
}

void Span::cancel() {
    cancelled_ = true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Point events
// ═══════════════════════════════════════════════════════════════════════════════

void emit(const char* category, const char* message,
          std::initializer_list<std::pair<const char*, const char*>> fields) {
    int64_t ts = std::time(nullptr);

    std::ostringstream json;
    json << "{\"ts\":" << ts
         << ",\"svc\":\"" << escapeJson(s_service) << "\""
         << ",\"cat\":\"" << escapeJson(category) << "\""
         << ",\"msg\":\"" << escapeJson(message) << "\""
         << ",\"type\":\"emit\"";

    if (fields.size() > 0) {
        json << ",\"fields\":{";
        bool first = true;
        for (const auto& [k, v] : fields) {
            if (!first) json << ",";
            json << "\"" << escapeJson(k) << "\":\"" << escapeJson(v) << "\"";
            first = false;
        }
        json << "}";
    }

    json << "}";
    sendDatagram(json.str());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Structured logging
// ═══════════════════════════════════════════════════════════════════════════════

static const char* levelTag(LogLevel level) {
    switch (level) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO";
        case WARN:  return "WARN";
        case ERROR: return "ERROR";
    }
    return "INFO";
}

void vlog(LogLevel level, const char* fmt, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);

    const char* tag = levelTag(level);

    // Always write to stderr for journald
    fprintf(stderr, "[%s] %s: %s\n", s_service.c_str(), tag, buf);

    // Also emit to traced
    int64_t ts = std::time(nullptr);
    std::ostringstream json;
    json << "{\"ts\":" << ts
         << ",\"svc\":\"" << escapeJson(s_service) << "\""
         << ",\"cat\":\"log\""
         << ",\"msg\":\"" << escapeJson(buf) << "\""
         << ",\"type\":\"emit\""
         << ",\"fields\":{\"level\":\"" << tag << "\"}"
         << "}";
    sendDatagram(json.str());
}

void log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level, fmt, args);
    va_end(args);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Trace ID
// ═══════════════════════════════════════════════════════════════════════════════

std::string generateTraceId() {
    static std::atomic<uint32_t> s_seq{0};
    auto ts = static_cast<uint32_t>(std::time(nullptr));
    auto pid = static_cast<uint16_t>(getpid());
    auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "t%08x%04x%04x", ts, pid, seq & 0xFFFF);
    return buf;
}

} // namespace trace
