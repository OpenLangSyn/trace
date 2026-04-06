# Changelog

## v2.1.0 — 2026-04-03

Open-source release under MIT license (library only).

- MIT license
- SPDX-License-Identifier headers in library source files
- 78 unit tests (lifecycle, emit, Span, log, trace ID, thread safety)
- README, CONTRIBUTING.md, CHANGELOG

## v2.0.0 — 2026-03-20

Complete v2 rewrite. Daemon-based architecture.

- Span (RAII scoped timer) with entry/exit events and measured duration
- Point events via emit()
- Structured logging to stderr + collector
- Unix datagram socket transport (fire-and-forget)
- Trace ID generation for cross-process correlation
- Graceful degradation when collector is unavailable

## Pre-release History

| Date | Change |
|------|--------|
| 2026-03-20 | T-03: Trace v2 daemon refactor (Phases 1-4). |
| 2026-03-18 | TRACE-05: Relay support (laptop-l1 → tower-l1). |
| 2026-03-18 | TRACE-04: Claude Code hooks + trace-emit CLI. |
| 2026-03-17 | TRACE-03: traced self-trace + config file. |
