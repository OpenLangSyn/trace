# Trace Library

Updated: 2026-04-06

## What It Is

A pure execution trace system. A daemon (`traced`) owns the trace stream —
services connect over a unix datagram socket and emit events. The daemon writes
to SQLite. No service touches the database directly.

## Location

- Source: `base/trace/`
- Header: `include/trace/Trace.h`
- Client library: `src/Trace.cpp`
- Daemon: `src/traced.cpp`
- Viewer: `tools/trace-viewer` (connects to daemon query socket)
- Tests: `tests/test_trace.cpp`
- CLI emitter: `tools/trace-emit`
- Compiled: `libtrace.a` (static library), `traced` (daemon binary)

Default paths (configurable via CLI flags):
- Recv socket: `/run/traced/traced.sock` (SOCK_DGRAM)
- Query socket: `/run/traced/traced-query.sock` (SOCK_STREAM)
- Database: `/var/lib/traced/trace.db`
- Config: `/etc/traced/traced.conf`

## API

Two primitives + structured logging:

```cpp
// Connect to traced daemon — call once at startup
trace::init("service-name");

// Scope — RAII, captures entry/exit + intent + duration
{
    trace::Span span("query", "BFS lookup — entity by name");
    span.set("entity", name);
    span.setTraceId(tid.c_str());
    span.markError();   // optional
    span.cancel();      // optional — suppress emission
}   // emits span_start on construction, span event with duration on scope exit

// Point event — decision, status, or observation
trace::emit("cache", "hit — skipping BFS", {{"key", hash}});

// Structured logging — stderr + traced
trace::log(trace::INFO, "started on port %d", port);

// Disconnect
trace::shutdown();
```

Service name is set once at `init()`, not repeated on every call.

### Span Lifecycle

Each Span has a unique `span_id` (auto-generated). On construction, a
`span_start` event is emitted (for open span tracking). On destruction:
- Normal: `span` event with duration + span_id (written to DB)
- Cancelled: `span_cancel` event (removes from tracking, NOT written to DB)

The daemon tracks open spans in memory for STATUS queries.

### Transport

Two listeners + optional relay:

1. **Unix datagram socket** (`/run/traced/traced.sock`) — local services
2. **UDP socket** (configurable port, `0.0.0.0`) — remote services
3. **Relay** (outbound UDP) — forward all events to upstream traced (fire-and-forget)

Fire-and-forget from service side. If daemon is down, sends silently fail —
services never block on trace.

### Relay

When `relay_host` is configured, traced forwards a copy of every valid incoming
event as a raw UDP datagram to the upstream traced. Events are stored locally
AND relayed. Fire-and-forget: if upstream is down, relay silently drops.

**Do not create relay loops** — relay is unidirectional (edge → collector).

Wire format: JSON datagram (same for all transports).
```json
{"ts":1710400000,"svc":"my-service","cat":"query","msg":"lookup","dur_ms":42,"type":"span","span_id":"s00010003","fields":{"entity":"foo"}}
```

## Daemon: traced

Single-threaded event loop (poll-based). Responsibilities:

1. Receive events on unix datagram socket
2. Write to SQLite
3. Track open spans in memory (span_start/span_cancel/span events)
4. Serve viewer queries on SOCK_STREAM query socket
5. Forward live events to connected viewers
6. Auto-prune (default 7 days, configurable via config or `-p`)
7. Prepared statement reuse for insert performance
8. Self-trace — emits own lifecycle events (service `traced`)

Options: `-s socket_path`, `-d db_path`, `-c config_path`, `-p prune_hours`,
`-u udp_port`, `-r host:port`.
Query socket path derived automatically: `<socket>-query.sock`.

### Config File

`/etc/traced/traced.conf` (key=value, `#` comments). CLI flags override config values.

| Key | Default | Description |
|-----|---------|-------------|
| prune_hours | 168 | Retention period in hours (7 days) |
| prune_interval | 3600 | Seconds between prune checks (1 hour) |
| udp_port | 0 | UDP port for remote events (0 = disabled) |
| relay_host | (empty) | Upstream traced IP (empty = disabled) |
| relay_port | 9300 | Upstream traced UDP port |

### Self-Trace

traced uses libtrace to instrument its own lifecycle. Events appear as
service `traced` in the trace DB:

| Category | Type | When |
|----------|------|------|
| lifecycle | emit | startup (with config values), shutdown (with counters) |
| prune | span | each prune cycle (rows_deleted, retention_hours, duration) |
| viewer | emit | viewer connect/disconnect (with active viewer count) |

### Query Protocol

Viewers connect to the query socket (SOCK_STREAM).
Commands (newline-terminated):

- `TAIL [json_options]\n` — backlog + live event stream
- `ERRORS [json_options]\n` — error-only backlog + live stream
- `STATUS\n` — current open spans (one-shot, then close)

Options JSON: `{"since":300,"service":"my-service","category":"query","limit":200}`

For TAIL/ERRORS: daemon sends backlog events (JSON lines), then
`{"__marker":"end_backlog"}\n`, then streams live events as they arrive.

For STATUS: daemon sends `{"spans":[...]}\n` and closes.

## Database Schema

**trace_events:**
```sql
id INTEGER PRIMARY KEY AUTOINCREMENT,
timestamp INTEGER NOT NULL,
service TEXT NOT NULL,
category TEXT NOT NULL,
message TEXT,
fields TEXT,           -- JSON object
duration_ms INTEGER DEFAULT 0,
trace_id TEXT DEFAULT '',
type TEXT DEFAULT 'emit'   -- 'span' or 'emit'
-- Indices on: timestamp, service, category, trace_id
```

## trace-viewer

Connects to traced daemon query socket. Three modes (Tab to cycle):
1. **tail** — Live event stream (newest last, color-coded by service)
2. **status** — Currently open spans per service (1s refresh)
3. **errors** — Error-only event stream

Filters: `-s SERVICE`, `-c CATEGORY`, `--since MINUTES`, `--limit N`.
`--json` flag for machine-readable output.
`--socket PATH` to override query socket path.

## Build

```bash
cd base/trace
make                      # Build libtrace.a + traced
make test                 # Build + run integration tests
sudo make install-daemon  # Install traced + systemd unit
sudo make install         # Install everything (library + daemon)
```

Link flags: `-ltrace -lpthread`

## Service Integration

```cpp
int main() {
    trace::init("my-service");
    // ...
    trace::shutdown();
}
```

## trace-emit CLI

General-purpose datagram sender for ad-hoc trace events from scripts:
```bash
trace-emit --svc my-service --cat test --msg "smoke test" --fields '{"k":"v"}'
```

## Interfaces

- **Library:** `libtrace.a` + `trace/Trace.h` (static, `-ltrace -lpthread`)
- **Daemon:** `traced` (unix sockets — recv + query; UDP socket — remote recv)
- **Timestamps are seconds since epoch** — use `datetime(timestamp, 'unixepoch', 'localtime')` in SQL
