# Unified socket Implementation Plan

> **For agentic workers:** Use test-driven development and independently review the implementation. The user has approved API B and requested implementation; do not ask them to select an API again.

**Goal:** Replace the three public socket role types with one lazy-opening, move-only `wavvix::net::socket`.

**Architecture:** Share ownership, validation, complete TCP transfers and datagram transfers in `src/net/net_socket.cpp`. Keep OS creation, binding, endpoint queries, accept and connect in `net_socket_posix.cpp` / `net_socket_win.cpp`, behind one private helper header. Retain the existing runtime I/O engine and worker affinity.

**Tech Stack:** C++11, xmake, native sockets, existing result/error and coroutine runtime; no dependencies.

**Spec:** API B approved in conversation; its exact public signatures and current behavioral contract are documented in `include/culib/net.h` and `docs/net.md` as part of this change. The 2026-10-08 net/UDP designs describe the superseded API.

## Global constraints

- Windows 10+, Linux and macOS; 64-bit only; no exceptions/RTTI dependency.
- Public headers have UTF-8 BOM, no platform headers and only ASCII string literals.
- Every if/for/while body has K&R braces. Use early returns and blank lines between logical sections.
- No old-type aliases, fallback implementation or parallel old API.
- No stack allocator changes, commits, pushes or CI dispatch in this task.

## 1. Consumer tests first

- [x] Migrate TCP and UDP tests to construction followed by member bind/listen/connect. Remove hand-written full-transfer loops in favor of `read_exactly` / `write`.
- [x] Add a small new contract test file and compile it before implementing: `socket s; EXPECT(s.is_open(), ==, false); EXPECT(s.bind("127.0.0.1", 0).is_ok(), ==, true);` must fail because `socket` does not exist yet.
- [x] Cover lazy creation, move/close/reuse, protocol misuse, bound clients, local/peer endpoint queries, hostname connection, default and zero timeouts, partial reads, complete reads/writes, premature EOF, shared total deadlines and failed-connection reuse.
- [x] Preserve IPv6 separation, TIME_WAIT rebind, worker affinity, UDP empty packets/truncation/timeout recovery and closed-handle tests.

## 2. Unified implementation

- [x] Replace public role classes with `protocol`, `datagram`, `socket`. Constructor defaults to TCP and only stores protocol. Default timeout is `nanoseconds(-1)`; every negative duration means unlimited.
- [x] Store native handle, owner, protocol and a small phase value (closed/bound/listening/connected). Phase validates public operations; do not introduce a cancellation/state-machine framework.
- [x] Combine platform resource operations under `net_socket_detail.h`. Preserve POSIX flags, IPv6-only, TCP SO_REUSEADDR, Windows overlapped operations, AcceptEx cleanup/context, ConnectEx local binding/context and UDP ICMP suppression.
- [x] `read` transfers some bytes; `read_exactly` and `write` loop with one steady-clock budget. On EOF before completion return `io`. Partial progress on errors is not rolled back and does not implicitly close a connected socket.
- [x] Endpoint connect creates on demand, accepts an explicitly bound socket of the same family, rejects a live listener/connection, and closes a failed connection attempt. Host connect resolves first, then uses one budget across candidates in system order. For an explicitly bound client, filter candidates by family and rebind the saved actual local endpoint before another attempt. Stop if rebinding fails; never silently replace its local binding with an unbound retry.
- [x] Offload resolve via existing `call_blocking`; outside a coroutine this is synchronous. DNS is not covered by the connection timeout.
- [x] UDP requires bind, retains message boundaries and returns void from successful send_to. Zero-byte packets remain valid; truncation consumes the packet and returns out_of_range.
- [x] Delete superseded implementation files after inspecting them; keep no compatibility layer.

## 3. Migrate consumers and documentation

- [x] Update both network examples, address example's resolver comments and runtime fault test. Current CI consumer code contains no old network calls.
- [x] Update `api.md`, `docs/net.md`, `docs/api-guide.md` and any current architecture/runtime claims. Label older specs as historical rather than rewriting their original contracts.
- [x] Document no-close-with-waiters, one-worker ownership, bound-host behavior, DNS timeout exclusion and irreversible partial transfer progress.

## 4. Verification and review

- [x] Build/tests/examples with Linux GCC and Clang debug/release, no-exceptions/RTTI and ASan+UBSan, plus Windows MSVC debug/release (pin xmake project and use -j 2 on Windows).
- [x] Run targeted repeat/stress tests; ensure new tests detect a reset-per-chunk deadline and partial-write implementation.
- [x] Independently review implementation and tests, resolve confirmed findings, and repeat affected checks.
- [x] Check standalone C++11 header/BOM, live old-type references and final diff. Deliver example source and actual output, exact test results and coverage gaps. Do not present prior CI as validation of this change.
