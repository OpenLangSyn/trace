/*
 * test_libtrace.cpp — Unit tests for libtrace (no daemon required).
 *
 * Creates a SOCK_DGRAM listener to capture JSON datagrams directly.
 *
 * Copyright (c) 2025-2026 Are Bjørby <are.bjorby@langsyn.org>
 * SPDX-License-Identifier: MIT
 */

#include <trace/Trace.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

// ═════════════════════════════════════════════════════════════════════════════
// Test framework
// ═════════════════════════════════════════════════════════════════════════════

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "    FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failed++; return; \
    } else { \
        g_passed++; \
    } \
} while (0)

#define ASSERT_EQ(a, b, msg) ASSERT_TRUE((a) == (b), msg)

#define TEST(name) static void name()
#define RUN(name) do { \
    fprintf(stderr, "  %s\n", #name); \
    name(); \
} while (0)

// ═════════════════════════════════════════════════════════════════════════════
// Test socket helper — receives JSON datagrams
// ═════════════════════════════════════════════════════════════════════════════

static const char* TEST_SOCK = "/tmp/libtrace_test.sock";

struct TestReceiver {
    int fd = -1;

    bool open() {
        unlink(TEST_SOCK);
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, TEST_SOCK, sizeof(addr.sun_path) - 1);
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd); fd = -1; return false;
        }
        return true;
    }

    // Read one datagram, timeout in ms. Returns empty on timeout.
    std::string recv(int timeout_ms = 500) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return {};
        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return {};
        buf[n] = '\0';
        return std::string(buf, n);
    }

    // Drain all pending datagrams
    std::vector<std::string> drain(int timeout_ms = 200) {
        std::vector<std::string> msgs;
        for (;;) {
            auto m = recv(timeout_ms);
            if (m.empty()) break;
            msgs.push_back(std::move(m));
        }
        return msgs;
    }

    void cleanup() {
        if (fd >= 0) { close(fd); fd = -1; }
        unlink(TEST_SOCK);
    }
};

// Simple JSON field extraction (no parser dependency)
static std::string jsonValue(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        // String value
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return json.substr(pos + 1, end - pos - 1);
    }
    // Numeric or other
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

static bool jsonContains(const std::string& json, const char* substring) {
    return json.find(substring) != std::string::npos;
}

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(test_connected_before_init) {
    ASSERT_TRUE(!trace::connected(), "not connected before init");
}

TEST(test_shutdown_before_init) {
    trace::shutdown();  // must not crash
    ASSERT_TRUE(!trace::connected(), "still not connected");
}

TEST(test_init_to_receiver) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    bool ok = trace::init("test-svc", TEST_SOCK);
    ASSERT_TRUE(ok, "init returns true");
    ASSERT_TRUE(trace::connected(), "connected after init");
    trace::shutdown();
    ASSERT_TRUE(!trace::connected(), "not connected after shutdown");
    rx.cleanup();
}

TEST(test_init_missing_socket) {
    // Init to a non-existent path — socket creation succeeds (DGRAM),
    // but there's no listener. Fire-and-forget means no error.
    bool ok = trace::init("test-svc", "/tmp/libtrace_nonexistent.sock");
    ASSERT_TRUE(ok, "init succeeds even without listener");
    ASSERT_TRUE(trace::connected(), "connected flag set");
    trace::shutdown();
}

TEST(test_double_init) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    bool ok1 = trace::init("svc1", TEST_SOCK);
    bool ok2 = trace::init("svc2", TEST_SOCK);  // should return true (already init)
    ASSERT_TRUE(ok1, "first init ok");
    ASSERT_TRUE(ok2, "second init ok (noop)");
    trace::shutdown();
    rx.cleanup();
}

TEST(test_double_shutdown) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("test-svc", TEST_SOCK);
    trace::shutdown();
    trace::shutdown();  // must not crash
    ASSERT_TRUE(!trace::connected(), "still not connected");
    rx.cleanup();
}

// ═════════════════════════════════════════════════════════════════════════════
// Emit tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(test_emit_basic) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("emit-test", TEST_SOCK);

    trace::emit("cache", "hit");

    auto msg = rx.recv();
    ASSERT_TRUE(!msg.empty(), "received datagram");
    ASSERT_EQ(jsonValue(msg, "svc"), "emit-test", "service name correct");
    ASSERT_EQ(jsonValue(msg, "cat"), "cache", "category correct");
    ASSERT_EQ(jsonValue(msg, "msg"), "hit", "message correct");
    ASSERT_EQ(jsonValue(msg, "type"), "emit", "type is emit");
    ASSERT_TRUE(!jsonValue(msg, "ts").empty(), "has timestamp");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_emit_with_fields) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("field-test", TEST_SOCK);

    trace::emit("lookup", "found", {{"key", "abc"}, {"depth", "3"}});

    auto msg = rx.recv();
    ASSERT_TRUE(!msg.empty(), "received datagram");
    ASSERT_TRUE(jsonContains(msg, "\"key\":\"abc\""), "field key present");
    ASSERT_TRUE(jsonContains(msg, "\"depth\":\"3\""), "field depth present");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_emit_no_fields) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("nf-test", TEST_SOCK);

    trace::emit("status", "idle");

    auto msg = rx.recv();
    ASSERT_TRUE(!msg.empty(), "received datagram");
    ASSERT_TRUE(!jsonContains(msg, "\"fields\""), "no fields key");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_emit_special_chars) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("esc-test", TEST_SOCK);

    trace::emit("test", "line1\nline2\ttab \"quoted\"");

    auto msg = rx.recv();
    ASSERT_TRUE(!msg.empty(), "received datagram");
    ASSERT_TRUE(jsonContains(msg, "\\n"), "newline escaped");
    ASSERT_TRUE(jsonContains(msg, "\\t"), "tab escaped");
    ASSERT_TRUE(jsonContains(msg, "\\\"quoted\\\""), "quotes escaped");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_emit_when_disconnected) {
    // Must not crash — silently drops
    trace::emit("orphan", "should be dropped");
    g_passed++;  // if we got here, it didn't crash
}

// ═════════════════════════════════════════════════════════════════════════════
// Span tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(test_span_basic) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("span-test", TEST_SOCK);

    {
        trace::Span span("query", "BFS lookup");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() >= 2, "at least 2 datagrams (start + end)");

    // First: span_start
    auto& start = msgs[0];
    ASSERT_EQ(jsonValue(start, "type"), "span_start", "first is span_start");
    ASSERT_EQ(jsonValue(start, "cat"), "query", "start category");
    ASSERT_EQ(jsonValue(start, "msg"), "BFS lookup", "start message");
    auto span_id = jsonValue(start, "span_id");
    ASSERT_TRUE(!span_id.empty(), "span_id present in start");

    // Second: span (exit)
    auto& end = msgs[1];
    ASSERT_EQ(jsonValue(end, "type"), "span", "second is span");
    ASSERT_EQ(jsonValue(end, "span_id"), span_id, "span_id matches");
    auto dur = jsonValue(end, "dur_ms");
    ASSERT_TRUE(!dur.empty(), "has duration");
    ASSERT_TRUE(std::stoi(dur) >= 20, "duration >= 20ms");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_span_fields) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("sf-test", TEST_SOCK);

    {
        trace::Span span("parse", "tokenize input");
        span.set("tokens", "42");
        span.set("lang", "en");
    }

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() >= 2, "got start + end");
    auto& end = msgs[1];
    ASSERT_TRUE(jsonContains(end, "\"tokens\":\"42\""), "tokens field");
    ASSERT_TRUE(jsonContains(end, "\"lang\":\"en\""), "lang field");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_span_trace_id) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("tid-test", TEST_SOCK);

    std::string tid = trace::generateTraceId();
    {
        trace::Span span("pipeline", "step 1");
        span.setTraceId(tid);
    }

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() >= 2, "got start + end");
    auto& end = msgs[1];
    ASSERT_EQ(jsonValue(end, "trace_id"), tid, "trace_id matches");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_span_error) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("err-test", TEST_SOCK);

    {
        trace::Span span("rpc", "call gate");
        span.markError();
    }

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() >= 2, "got start + end");
    auto& end = msgs[1];
    ASSERT_TRUE(jsonContains(end, "\"error\":\"true\""), "error field present");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_span_cancel) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("cancel-test", TEST_SOCK);

    {
        trace::Span span("route", "should be cancelled");
        span.cancel();
    }

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() >= 2, "got start + cancel");
    auto& cancel = msgs[1];
    ASSERT_EQ(jsonValue(cancel, "type"), "span_cancel", "type is span_cancel");
    ASSERT_TRUE(!jsonContains(cancel, "dur_ms"), "no duration on cancel");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_span_id_unique) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("uniq-test", TEST_SOCK);

    {
        trace::Span s1("a", "first");
    }
    {
        trace::Span s2("b", "second");
    }

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() >= 4, "4 datagrams (2 spans x start+end)");

    auto id1 = jsonValue(msgs[0], "span_id");
    auto id2 = jsonValue(msgs[2], "span_id");
    ASSERT_TRUE(id1 != id2, "span IDs are unique");

    trace::shutdown();
    rx.cleanup();
}

// ═════════════════════════════════════════════════════════════════════════════
// Log tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(test_log_emits_to_socket) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("log-test", TEST_SOCK);

    trace::log(trace::INFO, "started on port %d", 8891);

    auto msg = rx.recv();
    ASSERT_TRUE(!msg.empty(), "received log datagram");
    ASSERT_EQ(jsonValue(msg, "cat"), "log", "category is log");
    ASSERT_TRUE(jsonContains(msg, "started on port 8891"), "formatted message");
    ASSERT_TRUE(jsonContains(msg, "\"level\":\"INFO\""), "level field");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_log_levels) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("lvl-test", TEST_SOCK);

    trace::log(trace::DEBUG, "d");
    trace::log(trace::WARN, "w");
    trace::log(trace::ERROR, "e");

    auto msgs = rx.drain();
    ASSERT_TRUE(msgs.size() == 3, "3 log events");
    ASSERT_TRUE(jsonContains(msgs[0], "\"level\":\"DEBUG\""), "DEBUG level");
    ASSERT_TRUE(jsonContains(msgs[1], "\"level\":\"WARN\""), "WARN level");
    ASSERT_TRUE(jsonContains(msgs[2], "\"level\":\"ERROR\""), "ERROR level");

    trace::shutdown();
    rx.cleanup();
}

// ═════════════════════════════════════════════════════════════════════════════
// Trace ID tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(test_trace_id_format) {
    auto id = trace::generateTraceId();
    ASSERT_TRUE(id.size() == 17, "trace ID is 17 chars (t + 8 + 4 + 4)");
    ASSERT_TRUE(id[0] == 't', "trace ID starts with 't'");
}

TEST(test_trace_id_unique) {
    auto id1 = trace::generateTraceId();
    auto id2 = trace::generateTraceId();
    auto id3 = trace::generateTraceId();
    ASSERT_TRUE(id1 != id2, "id1 != id2");
    ASSERT_TRUE(id2 != id3, "id2 != id3");
}

// ═════════════════════════════════════════════════════════════════════════════
// Burst / thread safety
// ═════════════════════════════════════════════════════════════════════════════

TEST(test_burst_emit) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("burst-test", TEST_SOCK);

    for (int i = 0; i < 50; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "event %d", i);
        trace::emit("burst", msg);
    }

    auto msgs = rx.drain(500);
    ASSERT_EQ((int)msgs.size(), 50, "received all 50 burst events");

    trace::shutdown();
    rx.cleanup();
}

TEST(test_threaded_emit) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("thread-test", TEST_SOCK);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([t]() {
            for (int i = 0; i < 10; i++) {
                char msg[32];
                snprintf(msg, sizeof(msg), "t%d-e%d", t, i);
                trace::emit("mt", msg);
            }
        });
    }
    for (auto& t : threads) t.join();

    auto msgs = rx.drain(500);
    ASSERT_EQ((int)msgs.size(), 40, "received all 40 threaded events");

    trace::shutdown();
    rx.cleanup();
}

// ═════════════════════════════════════════════════════════════════════════════
// Main
// ═════════════════════════════════════════════════════════════════════════════

int main() {
    fprintf(stderr, "=== libtrace unit tests ===\n\n");

    fprintf(stderr, "--- Lifecycle ---\n");
    RUN(test_connected_before_init);
    RUN(test_shutdown_before_init);
    RUN(test_init_to_receiver);
    RUN(test_init_missing_socket);
    RUN(test_double_init);
    RUN(test_double_shutdown);

    fprintf(stderr, "\n--- Emit ---\n");
    RUN(test_emit_basic);
    RUN(test_emit_with_fields);
    RUN(test_emit_no_fields);
    RUN(test_emit_special_chars);
    RUN(test_emit_when_disconnected);

    fprintf(stderr, "\n--- Span ---\n");
    RUN(test_span_basic);
    RUN(test_span_fields);
    RUN(test_span_trace_id);
    RUN(test_span_error);
    RUN(test_span_cancel);
    RUN(test_span_id_unique);

    fprintf(stderr, "\n--- Log ---\n");
    RUN(test_log_emits_to_socket);
    RUN(test_log_levels);

    fprintf(stderr, "\n--- Trace ID ---\n");
    RUN(test_trace_id_format);
    RUN(test_trace_id_unique);

    fprintf(stderr, "\n--- Stress ---\n");
    RUN(test_burst_emit);
    RUN(test_threaded_emit);

    fprintf(stderr, "\n=== %d/%d passed ===\n", g_passed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
