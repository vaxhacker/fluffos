# Idle HTTP upload investigation

Investigated 2026-09-22–23 against
`dc910839e032a9336ac190362bb6188ac9f59cb0` on Linux, using the vendored
libwebsockets/libevent backend. The reported seconds-long stall did **not**
reproduce. This change adds regression coverage; it does not claim to fix the
reported production failure or change the HTTP implementation.

## Reproduction and controls

Run the complete HTTP integration suite:

```sh
python3 tools/test_http.py build-http/src/driver --log /tmp/fluffos-http.log
```

Run only the idle cases, optionally recording socket and event-loop syscalls:

```sh
python3 tools/test_http_idle_upload.py build-http/src/driver \
  --log-dir /tmp/fluffos-http-idle --trace
```

Each size starts a fresh driver without listeners or other requests. A one-shot
100 ms boot delay expires before the measured request starts. The game tick is
60 seconds away, beyond the 10-second process watchdog. The HTTP transport's own
events and the 5-second request deadline remain enabled.

The Linux fixture caps the actual TCP send buffer with `SO_SNDBUF=4096` and
counts real `poll(POLLOUT, 0)` choke results. It does not synthesize readiness,
write to the socket, or schedule driver events. The server advertises a small
receive buffer, waits 50 ms after reading headers, then continuously drains the
body. The test requires a choke, a matching body digest and byte count, HTTP 200,
and completion in less than 500 ms. Thus a fast loopback transfer that never
fills a socket buffer cannot silently count as coverage.

The fixture needs a C compiler and is explicitly skipped outside Linux.
`--trace` additionally needs strace. Run sanitizer checks without `--trace`,
because LeakSanitizer cannot inspect a process being traced.

| Driver / control | 200,000 bytes | 2,000,000 bytes | Result |
| --- | ---: | ---: | --- |
| Unmodified dc910839, traced | 52 ms | 60 ms | Pass, 2 real chokes each |
| dc910839 with only the `rearm()` writable request removed | 5-second timeout | 5-second timeout | Fail, 1 real choke each |
| Original `rearm()` restored; complete HTTP suite | 52 ms | 55 ms | Pass, 9 / 119 real chokes |
| Debug + GCC ASan/UBSan; complete HTTP suite | 52 ms | 56 ms | Pass, 9 / 119 real chokes |

These are control measurements, not a before/after performance improvement.
The required failure on unmodified dc910839 was not observed. Removing rearming
is a deliberately broken build used to verify test sensitivity; that mutation
is not included in the change.

Earlier probes using a 20 ms peer pause also passed with the extracted production
dc910839 binary (22 / 28 ms). Delaying libwebsockets' initial high-resolution
timer and both idle-timer schedules to 60 seconds still completed in 22 / 29 ms.
That diagnostic build retained normal writable rearming. All diagnostic source
changes were restored.

## What wakes the upload

The 200 KB baseline syscall trace contains this sequence, with timestamps
relative to the first shown choke:

```text
0 us       poll(socket, POLLOUT, timeout=0) -> not writable
27 us      epoll_ctl(MOD, socket, EPOLLIN | EPOLLOUT) -> success
52 us      epoll_pwait2(..., 1 ms) -> timer expires
1134 us    epoll_pwait2(..., 4.846 s) -> EPOLLOUT after 39.775 ms
41023 us   next body send
```

The writable watcher is armed immediately after choking. The long-timeout wait
returns on socket writability, before its timeout. A second choke similarly
rearms within 31 microseconds. The measured waits track TCP/peer readiness;
there is no multi-second gap waiting for periodic service in these runs.

This matches the source path:

1. [`http.cc`](../src/packages/http/http.cc): the zero-delay walltime callback
   invokes `rearm()`, which calls `lws_callback_on_writable()` on the driver's
   event-loop thread.
2. [`pollfd.c`](../src/thirdparty/libwebsockets/lib/core-net/pollfd.c): changing
   the poll mask directly invokes the event backend's `io()` operation with
   `LWS_EV_START | LWS_EV_WRITE`.
3. [`libevent.c`](../src/thirdparty/libwebsockets/lib/event-libs/libevent/libevent.c):
   `elops_io_event()` immediately calls `event_add()` on the existing
   `EV_WRITE | EV_PERSIST` watcher. Its callback services the socket through
   `lws_service_fd_tsi()`.

## Candidate changes evaluated

| Candidate | Finding |
| --- | --- |
| Call `lws_cancel_service()` after rearming | The API wakes the event loop, but the same-thread rearm already adds its writable watcher directly. No missing wakeup was found for it to repair. A wakeup also cannot make a blocked TCP socket writable. |
| Add a separate libevent `EV_WRITE` event on the socket | libwebsockets already owns this exact readiness watcher. A second watcher would require additional close, cancel, redirect and TLS lifecycle handling without an observed benefit. |
| Use the existing lws writable API from deferred driver work | This is already the implemented path, and both source inspection and the socket trace show prompt registration. Preserve it while retaining the stronger regression. |

The cancellation API contract is in the vendored
[`lws-service.h`](../src/thirdparty/libwebsockets/include/libwebsockets/lws-service.h).
No changes to timeout, response size, redirect, TLS, cancellation, or LPC callback
behavior are included.

## Integration evidence

The existing full HTTP suite passes before and after adding the idle cases,
including TLS, redirects, response limits, deadlines, and cancel/kill handling.

A clean dc910839 driver also passed `record_migrations` against the original
production snapshot using lpmudlib `f5ef6e7b0`: **19/19**, no `Record migration
attempt` retry lines, and zero runtime/catch log growth. This was a scratch
snapshot replay, not a change to the running world.

Local evidence from this investigation:

- `/tmp/fluffos-http-final-baseline/`: baseline driver logs, receive timings,
  and strace files.
- `/tmp/fluffos-http-rearm-disabled/`: failing negative control and traces.
- `/tmp/fluffos-http-after-full.log` and `.idle/`: restored implementation,
  complete HTTP suite and idle cases.
- `/tmp/fluffos-http-asan-full.log` and `.idle/`: Debug sanitizer suite and
  idle cases, with no sanitizer findings.
- `/tmp/fluffos-http-before-production-binary/`: extracted production driver.
- `/tmp/fluffos-http-before-service60/` and
  `/tmp/fluffos-http-service60-control.patch`: delayed-service control.
- `/tmp/fluffos-http-prod-clean.log`: clean production-snapshot replay.

The reported production failure remains unexplained. A failing production trace
would need to distinguish time before `http_request()`, delayed driver dispatch,
an unarmed writable watcher, and a socket that the kernel still reports blocked.
The local evidence does not justify choosing a runtime workaround among these.
