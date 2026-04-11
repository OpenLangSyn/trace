# libtrace

C++17 execution trace client library.

**Author:** Are Bjørby <are.bjorby@langsyn.org>

RAII scoped timers, point events, and structured logging over a unix datagram
socket. Services emit trace events to a collector daemon — they never touch
the database directly.

## Build

```bash
make lib          # Build libtrace.a + libtrace.so
make unit-test    # Build and run all 78 unit tests
make install-lib  # Install library + header to /usr/local (sudo required)
make clean        # Remove build artifacts
```

Requires: `g++` with C++17 support. Linux only (unix datagram sockets).

## API

Single public header: `#include <trace/Trace.h>`

```cpp
trace::init("my-service");                       // Connect to collector

{
    trace::Span span("query", "BFS entity lookup");
    span.set("entity", name);
    // ... work ...
}   // emits scope-exit event with measured duration

trace::emit("cache", "hit", {{"key", hash}});    // Point event

trace::log(trace::INFO, "started on port %d", p); // stderr + collector

trace::shutdown();                                // Disconnect
```

## Design

- **Two primitives**: `Span` (RAII scoped timer) and `emit` (point event).
- **Structured logging**: `log()` writes to both stderr (journald) and the collector.
- **Fire-and-forget**: Datagram socket — send never blocks, lost events are acceptable.
- **Graceful degradation**: If the collector is down, `init()` returns false and tracing degrades to stderr-only.
- **Trace IDs**: `generateTraceId()` for cross-process correlation.
- **Thread-safe**: All operations are mutex-protected.

Link flags: `-ltrace -lpthread`

## Collector

This library sends events to a collector daemon listening on a unix datagram socket
(default: `/run/traced/traced.sock`). The collector is responsible for persistence
(e.g., SQLite) and query serving. The collector daemon is not included in this
open-source release.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for coding standards and how to contribute.

## License

MIT License — see [LICENSE](LICENSE).
