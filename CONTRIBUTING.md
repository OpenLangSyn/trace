# Contributing to libtrace

## Architecture

```
trace/Trace.h (single public header)

Namespace: trace
  init(service, socket)    Connect to collector daemon
  shutdown()               Disconnect
  connected()              Connection status

Class: Span (RAII)
  Span(scope, intent)      Start scoped timer
  ~Span()                  Emit exit event with duration
  set(key, value)          Attach field to span
  setTraceId(id)           Set correlation ID
  markError()              Flag span as error
  cancel()                 Suppress emission

  emit(cat, msg, fields)   Point event
  log(level, fmt, ...)     Structured logging (stderr + collector)
  generateTraceId()        Random trace ID for correlation
```

## Source Files

| File | Purpose |
|------|---------|
| Trace.cpp | Client library: socket connection, JSON serialization, Span lifecycle |

## Build

```bash
make lib          # Build libtrace.a + libtrace.so
make unit-test    # Build and run all 78 unit tests
make install-lib  # Install library + header to /usr/local (sudo)
make clean        # Remove build artifacts
```

## Coding Standards

- **C++17**, compiled with `-Wall -Wextra -Werror -O2 -fPIC`
- **No external dependencies** — libc, libpthread only
- **Linux only** — unix datagram sockets
- **Single public header**: `trace/Trace.h`
- Fire-and-forget transport — never block the caller
- Thread-safe: all socket operations under mutex
- No commented-out code, no bare TODOs, no debug prints

## Adding Tests

Tests use a SOCK_DGRAM listener to capture JSON datagrams (`tests/test_libtrace.cpp`):

```cpp
TEST(test_my_feature) {
    TestReceiver rx;
    ASSERT_TRUE(rx.open(), "receiver opened");
    trace::init("test-svc", TEST_SOCK);

    trace::emit("cat", "msg");

    auto msg = rx.recv();
    ASSERT_TRUE(!msg.empty(), "received datagram");
    // ... verify JSON content ...

    trace::shutdown();
    rx.cleanup();
}
```

Register in `main()` with `RUN(test_my_feature)`.

## Test Suite

78 tests covering: lifecycle (init/shutdown/connected), emit (basic, fields, escaping,
disconnected), Span (start/end, fields, trace ID, error, cancel, uniqueness), log
(format, levels), trace ID (format, uniqueness), stress (burst, multi-threaded).

## Before Submitting

- Run `make clean && make unit-test` — all 78 tests must pass
- New public API requires documentation in `Trace.h` header comments
