# Distributed tracing through hogd

Status: design draft, 2026-10-01. Comments wanted before any implementation.

This is phase 3 of devnet tracing. The earlier phases stop at hogd's x23
boundary:

- **Phase 1:** Grafana Beyla HTTP spans through the host Alloy to Grafana Cloud
  Traces (groundhogchain/ops).
- **Phase 2:** in-process OpenTelemetry in `hog-gateway`, `hog-faucet`,
  `chuckd` and `scpd`.

## Goal

One trace per sampled request, across processes:

- a public JSON-RPC call: nginx → `hog-gateway` → `hog-mempool` / `hog-db` /
  `hog-exec` / `hog-indexer`;
- a block: `chuckd` propose and commit → `hog-mempool` → `hog-exec` /
  `hog-db` / `hog-event`.

Each hop is a child span with the procedure name and its latency. The
unsampled path must cost close to nothing, and all of it switches off for perf
reviews.

## Why phases 1 and 2 stop at hogd

- **No carrier.** Every inter-process call into or out of a hogd daemon is
  x23 RPC. The header (`x23/x23/rpc_msg.x`, RFC 5531) has nowhere today that
  carries trace context.
- **No tracing in hogd.** There is no tracing library or exporter.
- **Coroutines.** hogd handlers are C++20 coroutines (`MaybeAsync`/`Async`)
  resumed on `x23::ThreadPool` workers. Context kept in a thread-local, which
  is what the OTel C++ SDK's `RuntimeContext` does by default, is wrong after
  the first `co_await` resumes on another thread.

## The carrier: the call header's `cred`

`call_body` already has `opaque_auth cred`: an `auth_flavor` plus
`opaque body<400>`. Every x23 client sets `AUTH_NONE` with an empty body
(`rpc_common.h:73-74`). Servers never read `cred`; `SrvMap::lookup` checks
only `rpcvers`, `prog` and `vers`. A W3C `traceparent` is 55 bytes and
`tracestate` is optional, so the context fits without changing the message
layout or any program definition.

Two encodings:

| | A: new flavour `AUTH_GH_TRACE` | B: `AUTH_NONE` with a non-empty body |
|---|---|---|
| Wire change | Adds an enum value to `rpc_msg.x` | None |
| Old receivers | Accept it: decoders do not check that an enum value is a declared variant | Accept it, and ignore the body |
| Rollout | One step | One step |
| Semantics | Clean and explicit | Bends RFC 5531, which says `AUTH_NONE`'s body is empty |

**Recommendation: A.** Unknown `auth_flavor` values are already accepted, so
no decode-first release is needed. Still confirm it once per stack with a
cross-implementation test.

**Body format:** a version byte, then `traceparent`, then optionally
`tracestate`, so the format can evolve. A receiver that cannot parse the body
ignores it and starts a new root (or does nothing when unsampled). It never
rejects the call.

### Allocation on the hot path

`opaque body<400>` is variable-length, so a populated body costs an
allocation per call to encode and another to decode. To keep that off the hot
path:

- **Unsampled calls**, the common case, keep sending `AUTH_NONE` with an empty
  body. The wire bytes and the work are what they are today: no allocation.
- **Only sampled calls**, about 5% by default, carry `AUTH_GH_TRACE` and pay
  the ~26-byte body.
- **If that still shows up in the benchmark-regression numbers**, change the
  structure itself. For example, replace the credential's variable opaque with
  a fixed-size trace field (16-byte trace id, 8-byte span id, flags, all zero
  when tracing is off), or give the C++ decoder a small inline buffer so a
  body this size never reaches the heap. This departs from the RFC 5531
  layout, so it needs agreement from x23's maintainers. Decide on measured
  numbers, not in advance.

## Implementations to change

Each hogd peer has its own x23 RPC stack. Enumerate them from the code before
starting; do not rely on this list.

| Stack | Where | Role |
|---|---|---|
| C++ x23 | `hogd/x23/x23/rpc*.{h,cc}`, `rpc_common.h` | server and client in every hog-* daemon |
| Rust | `groundhog-rpc` x23 backend crates (`crates/x23/...`) and the `rpc_driver` crate (private registry) | the gateway's and faucet's calls into hogd |
| Rust | `scp`, through `rpc_driver` | scpd's side of chuckd ↔ scpd, which is XDR RPC |
| Go | chuckd's XDR RPC stack, via `groundhog-go` / `protocol-go` | chuckd → hog-mempool (`SchedulerCoord1`), and chuckd ↔ scpd |

Because chuckd ↔ scpd uses the Rust and Go XDR RPC implementations, scpd
joins the block trace once those two stacks carry the credential. No hogd
change is needed for it.

## hogd design

1. **Context travels with the coroutine, not the thread. Reuse corotrace's
   plumbing, not corotrace.**

   x23 already has coroutine tracing: `x23/x23/corotrace.{h,cc}`, behind
   `X23_CORO_TRACE`.
   - **What it records:** every `PromiseBase` gains creation and suspension
     `source_location`s and a `waiter_` link to the coroutine awaiting it.
     Every suspension point updates them (`Awaitable::await_suspend` in
     `async.h`, the semaphore and mutex awaiters in `corosync.h`).
     `CoroHandle` and `AtomicCoroHandle` carry the `PromiseBase*` across
     threads.
   - **Why it can't be the vehicle:** its cost is too high to run all the
     time, and CMake enables it only for Debug builds. It also tracks far more
     than a trace needs.

   So trace context gets separate, minimal machinery, attached at the same
   points corotrace already uses:
   - **Storage.** One pointer, `const TraceCtx *trace_`, in `PromiseBase`.
     It lives in the variant compiled for every build, behind a new flag
     (`X23_TRACE_CONTEXT`, default on in Release) rather than
     `X23_CORO_TRACE`. It is null unless the coroutine belongs to a sampled
     trace, so unsampled frames grow by 8 bytes and pay a null check.
     `TraceCtx` (trace id, parent span id, flags) is reference-counted and
     allocated once per sampled server call.
   - **Where context can be read: the awaiter.** An awaiter's
     `await_suspend(h)` sees the awaiting coroutine's promise. That is how
     corotrace sets `waiter_` (`async.h`, `Awaitable::await_suspend`), and how
     `GetCoroTrace` reads the current promise without really suspending.
   - **The catch: there is no single resume point.**
     - Coroutines resume through three explicit `resume()` calls in
       `async.h`.
     - They also resume through symmetric transfer, when `await_suspend`
       returns a handle, which offers no hook.
     - And through `resume_in_another_thread(CoroHandle{h, nullptr})` in
       `corosync.h`, which drops the promise pointer.

     So a thread-local "current context" kept up to date on every resume is
     not reliable. Do not build on one.
   - **The catch with eager start.** x23's `Async` starts eagerly
     (`initial_suspend` returns `suspend_never`). A child coroutine's body
     runs before its parent reaches `co_await`, so copying context into the
     child at `await_suspend` arrives after any work the child does before
     its first suspension.

   The design that fits both constraints:
   - **Server root.** `RpcDispatcher::serve` decodes `cred` and sets
     `trace_` on the handler's promise.
   - **Inheritance down awaited chains.** At the `await_suspend` sites where
     corotrace records `waiter_`, a child with no context copies its waiter's
     `trace_`. That covers everything the child does after its first
     suspension.
   - **Client side reads the caller, not the callee.** The outbound call
     takes its context from the coroutine that is calling, through the call
     awaiter's `await_suspend(h)`, and encodes `cred` there. This is correct
     only if x23's client builds and sends the call message from an awaiter's
     `await_suspend` (or later), not from an eagerly started coroutine body.
     **Reviewer question:** where does the x23 client send today? If it sends
     from an eager body, restructuring the send into the awaiter is the core
     x23 change here.
   - **Handler code changes:** none, if the client point above holds.
   - **Alternative, if that point does not hold:** pass context explicitly.
     Handlers forward a `TraceCtx` into the x23 client calls they make. That
     is simple and cheap, but it touches every handler that calls another
     daemon.
   - **Measure** `X23_TRACE_CONTEXT=1` against `=0` with the
     benchmark-regression scripts before turning it on by default.
2. **Spans.**
   - **Server:** one per served call, named `<program>/<proc>` with the same
     names as the eBPF uprobe metrics (`SchedulerCoord1/commit_block`, …).
     Start at dispatch and end at reply enqueue: the same two points the
     uprobes pair, so durations are comparable.
   - **Client:** one per outbound call.
   - **Optional:** a few internal spans around known heavy stages, such as
     `close_block`'s stages, which are already logged with timings.
   - **Hot procedures:** per-transaction procedures such as
     `DatabaseToExecutorInterfaceV1/get_snapshot_item` run at thousands per
     second. Spans for them exist only inside a sampled trace, and a cap of N
     child spans per parent span keeps a sampled block trace bounded.
3. **Export. Decide before implementing:**
   - **opentelemetry-cpp** with the OTLP/HTTP protobuf exporter to Alloy's
     `:4318`. This is the standard route, but it adds protobuf and libcurl or
     an HTTP client to the build. Measure link time and binary size on the
     znver5 Release build.
   - **A minimal exporter written in x23.** Batch spans in a lock-free ring and
     post OTLP/JSON to `:4318` from one background thread. There are no new
     dependencies and hogd keeps full control of hot-path cost, but we
     maintain it.

   Recommendation: prototype the minimal exporter first, measuring overhead
   with the benchmark-regression scripts. Adopt opentelemetry-cpp only if the
   prototype's maintenance cost looks worse than the dependency.
4. **Control.** The same env vars as phase 2: `OTEL_EXPORTER_OTLP_ENDPOINT`
   (off when unset), `OTEL_TRACES_SAMPLER` / `OTEL_TRACES_SAMPLER_ARG`
   (default `parentbased_traceidratio` 0.05, decided at the root by the
   gateway or chuckd), and `OTEL_SDK_DISABLED`. hogd never starts a sampled
   root on its own work unless that is explicitly enabled, so it follows the
   caller's decision.

## Order

1. **Senders.** `hog-gateway`, `hog-faucet` and `chuckd` put their current
   context into `cred` on XDR RPC calls, as `AUTH_GH_TRACE` on sampled calls
   only. scpd does the same through `rpc_driver`. Receivers already accept
   the unknown flavour; prove it with a cross-implementation test before
   enabling it.
2. **hogd.** The context pointer, its inheritance at the `await_suspend`
   sites, server and client spans, the exporter and the env controls. All of it sits behind
   `OTEL_EXPORTER_OTLP_ENDPOINT` at runtime and `X23_TRACE_CONTEXT` at build
   time.
3. **ops.** Set the OTel env in the validator compose for hog-core and the
   coordinator, pointed at the host Alloy receiver (`0.0.0.0:4317` on the
   validator since phase 1). It goes out with the promotion that carries
   step 2.
4. **Verify on devnet.**
   - A sampled `groundhog_sendRawTransaction`, or the nearest submit method,
     shows gateway → mempool → exec / db spans in one trace.
   - A sampled block shows chuckd → mempool `commit_block` → db and event
     spans.
   - Span durations agree with the eBPF `rpc_duration_seconds` histograms to
     within the histogram's power-of-two bucket.

## Acceptance

- **At the default 5% sampling,** hogd benchmark CPU and latency are within
  1% of the same build with tracing unset. Use the existing
  benchmark-regression scripts.
- **With tracing unset or `OTEL_SDK_DISABLED=true`,** there is no measurable
  difference from today's binaries.
- **Perf-review mode** (`scripts/devnet-telemetry.py --mode perf-review` in groundhogchain/ops)
  stops every span export.
- **Old peers keep working throughout**, at every step of the rollout.

## Resolved questions

- **Unknown `auth_flavor` values:** accepted. Decoders do not check that an
  enum value is a declared variant, so flavour A rolls out in one step.
- **chuckd ↔ scpd:** XDR RPC, through the Rust and Go implementations, so
  scpd joins via those stacks.
- **corotrace:** not used directly, because its overhead is too high to leave
  on. Its attachment points (`PromiseBase`, `CoroHandle`/`AtomicCoroHandle`)
  are reused for a pointer-sized context.

## Open questions

1. Which exporter, opentelemetry-cpp or the minimal one? Decide after the
   prototype measurements in step 2.
2. Where does the x23 client build and send a call message: an awaiter's
   `await_suspend`, or an eagerly started coroutine body? This decides
   whether context can be read implicitly from the caller or must be passed
   explicitly.
4. Is one pointer per coroutine frame (`X23_TRACE_CONTEXT=1`) within budget
   on the benchmark-regression numbers?
3. If sampled-call allocation for the `cred` body shows up in the
   measurements, do x23's maintainers accept a fixed-size trace field in place
   of the RFC 5531 `opaque_auth`, or an inline-buffer decoder?

## Not in scope

- Tail sampling.
- Linking traces to Pyroscope profiles through span-profile correlation.
  Revisit once hogd spans exist.
