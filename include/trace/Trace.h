#pragma once
/*
 * Trace.h — Pure execution trace library.
 *
 * Copyright (c) 2025-2026 Are Bjørby <are.bjorby@langsyn.org>
 * SPDX-License-Identifier: MIT
 *
 * Trace v2 — Pure execution trace library.
 *
 * Services call trace::init() once at startup to connect to the traced
 * daemon over a unix datagram socket. All recording goes through the
 * daemon — services never touch SQLite.
 *
 * Two primitives:
 *   trace::Span  — RAII scoped timer (entry/exit + intent + duration)
 *   trace::emit  — point event (decision, status, observation)
 *
 * Usage:
 *   trace::init("my-service");
 *
 *   {
 *       trace::Span span("query", "BFS lookup — entity by name");
 *       span.set("entity", name);
 *       // ... work ...
 *   }   // emits scope-exit event with duration
 *
 *   trace::emit("cache", "hit — skipping BFS", {{"key", hash}});
 *
 *   trace::log(trace::INFO, "started on port %d", port);
 *
 *   trace::shutdown();
 */

#include <string>
#include <initializer_list>
#include <utility>
#include <map>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <atomic>

namespace trace {

// ── Connection ──────────────────────────────────────────────────────────────

// Connect to traced daemon. Call once at startup.
// service_name: identifies this process in all trace output.
// socket_path: unix datagram socket where traced listens.
// Returns false if socket creation fails (tracing degrades to stderr-only).
bool init(const std::string& service_name,
          const std::string& socket_path = "/run/traced/traced.sock");

// Disconnect. Call at shutdown. Safe to call if init() was never called.
void shutdown();

// True if init() succeeded and socket is connected.
bool connected();

// ── Span ────────────────────────────────────────────────────────────────────

class Span {
public:
    // scope: what part of the code (e.g. "query", "pipeline", "parse")
    // intent: what and why in human terms
    Span(const char* scope, const char* intent);
    Span(const std::string& scope, const std::string& intent)
        : Span(scope.c_str(), intent.c_str()) {}
    ~Span();  // emits event with measured duration

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    void set(const char* key, const char* value);
    void set(const std::string& key, const std::string& value) {
        set(key.c_str(), value.c_str());
    }
    void setTraceId(const char* id);
    void setTraceId(const std::string& id) { setTraceId(id.c_str()); }
    void markError();
    void cancel();  // suppress emission on scope exit

private:
    std::string scope_;
    std::string intent_;
    std::string trace_id_;
    std::map<std::string, std::string> fields_;
    std::string span_id_;
    std::chrono::steady_clock::time_point start_;
    bool cancelled_ = false;
    bool error_ = false;
};

// ── Point events ────────────────────────────────────────────────────────────

void emit(const char* category, const char* message,
          std::initializer_list<std::pair<const char*, const char*>> fields = {});

// ── Structured logging ──────────────────────────────────────────────────────
// Writes to stderr (for journald) AND emits to traced (for trace.db).

enum LogLevel { DEBUG, INFO, WARN, ERROR };

void log(LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

void vlog(LogLevel level, const char* fmt, va_list args);

// ── Trace ID ────────────────────────────────────────────────────────────────

std::string generateTraceId();

} // namespace trace
