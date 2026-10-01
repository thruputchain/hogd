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
| Old receivers | Probably reject the frame when decoding the unknown enum value. Verify in each implementation. | Already decode it, and ignore the body |
| Rollout | Two steps: every peer learns to decode the flavour, then senders enable it | One step: senders can enable whenever ready |
| Semantics | Clean and explicit | Bends RFC 5531, which says `AUTH_NONE`'s body is empty |

**Recommendation: A, with the two-step rollout.** B stays the fallback if a
peer implementation cannot be updated in time.

**Body format:** a version byte, then `traceparent`, then optionally
`tracestate`, so the format can evolve. A receiver that cannot parse the body
ignores it and starts a new root (or does nothing when unsampled). It never
rejects the call.

## Implementations to change

Each hogd peer has its own x23 RPC stack. Enumerate them from the code before
starting; do not rely on this list.

| Stack | Where | Role |
|---|---|---|
| C++ x23 | `hogd/x23/x23/rpc*.{h,cc}`, `rpc_common.h` | server and client in every hog-* daemon |
| Rust | `groundhog-rpc` x23 backend crates (`crates/x23/...`) and the `rpc_driver` crate (private registry) | the gateway's and faucet's calls into hogd |
| Go | chuckd's x23 client, via `groundhog-go` / `protocol-go` | chuckd → hog-mempool (`SchedulerCoord1`) |
| Rust | `scp`'s `rpc_driver` use | chuckd ↔ scpd, if that path is x23 |

## hogd design

1. **Context travels with the coroutine, not the thread.**
   - Add a small `x23::TraceCtx` (trace id, span id, flags) to the coroutine
     promise of x23's `Async`/`MaybeAsync`.
   - When one task `co_await`s another, the child inherits the parent's
     context.
   - The server sets the root task's context from the incoming `cred` in
     `RpcDispatcher::serve`.
   - The client copies the current task's context into `cred` when it builds
     a call.
   - Handler code changes nowhere. Everything lives in x23.
   - The unsampled check is a flag test before any allocation.
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

1. **Decode support everywhere** (flavour A). Every stack decodes
   `AUTH_GH_TRACE` and ignores it. Release each component and promote one
   devnet release with all of them, so every peer can decode the new flavour
   before anyone sends it.
2. **Senders.** `hog-gateway`, `hog-faucet` and `chuckd` put their current
   context into `cred` on x23 calls. hogd still ignores it.
3. **hogd.** The coroutine context, server and client spans, the exporter and
   the env controls, behind `OTEL_EXPORTER_OTLP_ENDPOINT`.
4. **ops.** Set the OTel env in the validator compose for hog-core and the
   coordinator, pointed at the host Alloy receiver (`0.0.0.0:4317` on the
   validator since phase 1). It goes out with the promotion that carries
   step 3.
5. **Verify on devnet.**
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

## Open questions

1. Does each x23 decoder reject an unknown `auth_flavor` value? This decides
   whether flavour A needs step 1 or can go straight to step 2.
2. Is chuckd ↔ scpd x23, or another transport? If x23, scpd joins the block
   trace in step 2.
3. Which exporter, opentelemetry-cpp or the minimal one? Decide after the
   step 3 prototype measurements.

## Not in scope

- Tail sampling.
- Linking traces to Pyroscope profiles through span-profile correlation.
  Revisit once hogd spans exist.
