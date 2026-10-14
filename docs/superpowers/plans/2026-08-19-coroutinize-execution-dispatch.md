# Coroutinizing the execution dispatch path

Date: 2026-08-19. Goal: delete the async-thread → sync-thread handoff in
hog_exec (`DispatchPool`) now that wasm is gone (#3465), by turning the
execution spine into coroutines and switching the hogexec→hogdb client from
`x23::SRpcMulticlient` to the async `x23::RpcMulticlient`.

## Where the handoff is today

`ExecutorContext::propose_transaction` /
`propose_transaction_batch` / `validate_transaction{,_batch}` /
`view_transaction` are `x23::Async<...>` coroutines whose entire body is
`worker_pool_.submit(...)` + `co_await awaitable`
(`hogd/hog_exec/interface/executor_context.cc:53,67,87,102,125`). The submitted
job runs the whole transaction synchronously on a `DispatchPool` worker
(`hogd/hog_exec/interface/dispatch_pool.h`), and every DB read inside it blocks
that thread on a synchronous RPC
(`hogd/hog_exec/runtime/item/database_proxy.h:100`, `x23::SRpcMulticlient`).
`ViewGatewayRpc` has a second copy of the same structure
(`hogd/hog_exec/interface/rpc_from_view_gateway.cc:82,104`).

The thread pool exists *because* the RPC is synchronous. Nothing else in the
executor needs it.

## The spine that has to become awaitable

```
ExecutorContext::propose_transaction                 executor_context.cc:47
  run_propose_transaction                            executor_context.cc:152
    SystemCallHandler::root_invoke                   syscall_handler.cc:325
      Runtime::invoke                                runtime.cc:14
        Rv32Runtime::invoke_internal                 rv32_contract.cc:61   <- ecall loop
          syscall_entry                              syscall_entry.cc:86
            wrap_outer<Meta>                         syscall_entry_outer.h:731
              wrap_after_parse<Meta>                 syscall_entry_outer.h:489
                SyscallMeta::kInnerFunc              (generated)
                  SystemCallHandler::<storage op>    syscall_handler.cc
                    StorageSnapshotCache::try_get_*  storage_snapshot_cache.cc
                      DatabaseProxy::get_*           database_proxy.cc  <- blocks here
```

Cross-contract `Invoke` re-enters at `SystemCallHandler::invoke` →
`runtime->invoke(...)` (`syscall_handler.cc:892`), so the recursion is
coroutine recursion once the spine is coroutinized — bounded by the existing
frame-depth cap, one heap frame per nested call.

## Is "make syscall_entry return MaybeAsync" the right first step?

Yes as the *seam*, with three qualifications.

1. **It is a no-op on its own, and that is the point.** Land it while every
   inner syscall still returns a plain `std::pair`: `MaybeAsync` is always in
   its value arm, callers `operator()()` it, behavior is bit-identical. That
   makes it a reviewable NFC change and lets inner syscalls migrate one at a
   time afterwards instead of in one flag-day diff.

2. **`syscall_entry` is not the only seam.** Three callers consume the syscall
   ABI. All three block on the result in the first PR; only the first two ever
   stop blocking:
   - `Rv32Runtime::invoke_internal` (`rv32_contract.cc:100`) — the production
     path. Already shaped correctly for step 2: `ThreadedMachine` suspends at
     the ecall and returns to the loop, so no interpreter C++ stack is live
     across the syscall, and the loop becomes a coroutine cleanly.
   - `NativeSyscallContext::syscall<>` (`native_syscall_context.h:51`) — the
     precompiles. `precompile_native_token.cc:151,183` issue
     `NonnegativeIntGet` / `NonnegativeIntSetAdd`, i.e. real DB reads on the
     payment hot path. So at step 2 `NativeContract::function_t`
     (`native_runtime.h:52`, currently
     `UniqueFunction<InvokeStatus<void>(NativeSyscallContext&) const noexcept>`)
     has to become an `Async`-returning signature and all three precompiles
     become coroutines. That is the second-largest chunk of work after the
     syscall wrapper itself.
   - `mock_syscall_entrypoint` (`test_ffi/mock_syscall_entry.h:53`) — the
     `extern "C"` ABI the Rust SDK tests call. It stays synchronous and blocks
     with `MaybeAsync::operator()()` for good; test_ffi's DB is a local mock,
     so its bodies never suspend and the value arm is always taken.

   `rv32::RuntimeContext` (`rv32/runtime_context.cc`) calls `syscall_entry`
   from *inside* `machine.run(host)` and cannot suspend — but its header says
   "Experimental test-only", so it can keep blocking via `operator()()`.
   `SyscallFunction` (`runtime_exports.h`) is only used by that test context.

3. **The wrapper allocates a frame per syscall, and that is the accepted cost
   for now.** A coroutine always allocates, and wrap_after_parse() has to be
   one: its epilogue (output tracing, gas accounting, the kCanReturn Exit
   override, disarm_outputs, finish_syscall_trace) runs after the body's
   result, and the parsed-argument tuple the body writes through cannot be
   relocated while a suspended body holds references into it. So the tuple and
   the epilogue live in one frame, and the frame is created whether or not the
   body actually suspends. At a few dozen syscalls per transaction that is
   worth measuring before optimizing.

   Inner syscall bodies do *not* change: a body returning a plain
   SyscallInnerResult lands in MaybeAsync's value arm, and the co_await on it
   is free. A body that needs to wait switches its declared return type to
   MaybeAsyncSyscallInnerResult, one at a time.

   If the per-syscall frame does show up, the fix is to make the awaited thing
   a bespoke awaiter that *owns* the argument tuple and runs the body in
   await_ready(): it lives in the caller's MaybeAsync local, so nothing is
   heap-allocated unless the body really suspends. That makes the wrapper lazy
   (the body no longer runs at the call), which is why it is not the first
   move.

## Blocking is only legal off the x23 pool

`Async::operator()()` — the "run to completion, park this thread" form the
incremental strategy leans on — begins with `x23assert(!in_thread_pool())`
(`x23/async.h:586`). `SRpcStream::send_call` carries the identical assert,
commented "you tried to make a blocking RPC call from within a coroutine,
which can cause deadlock" (`x23/srpc.h:333-336`) — which is precisely why the
`DispatchPool` has to be a separate pool from x23's `ThreadPool` today.

Two consequences:

- The incremental plan works. Until the last step execution still runs on
  `DispatchPool` workers, which are not x23 pool threads, so any not-yet-
  coroutinized caller may legally block on a `MaybeAsync`.
- At the last step that stops being true, and every remaining blocking call becomes a
  bug. The assert is a useful net but only in debug: `x23assert` compiles to
  nothing under `NDEBUG` (`x23/debug.h:206`), so in a release build a leftover
  blocking call silently consumes a pool worker instead of panicking. Do the
  step-5 cutover against a debug build first, and grep for `operator()()` on
  the exec path before flipping.

## The `noexcept` boundary

`wrap_outer` is `noexcept` today and a lot of `syscall_entry_outer.h` exists to
keep it that way (#3400, #2658, #2699, #2730, #2768). A coroutine's frame
allocation happens in the ramp function and can throw `std::bad_alloc` before
the body runs; x23's promises do not define
`get_return_object_on_allocation_failure`, so a `noexcept` coroutine would
`std::terminate()` exactly where the current code carefully returns
`HostFnErrorCode::Unrecoverable`. The async arm therefore needs a non-noexcept
coroutine invoked from inside a `try` in the noexcept `wrap_outer`, translating
a failed frame allocation into `Unrecoverable` the same way the inner-body
boundary does.

## Prerequisite: thread-local state that assumes no interleaving

Today a transaction owns its DispatchPool thread start to finish. Once
transactions are coroutines, two of them interleave on one thread at every
suspension point.

- **`PerTransactionDatabaseProxy::current_subtransaction_batch_`
  (`item/database_proxy.h:192`) was `inline static thread_local` and is broken
  by this.** `add_delta` accumulates into it and `try_push_current_subtransaction`
  clears it after the RPC returns (`:160-162`); a second transaction resuming
  on the same thread during that RPC clobbers the batch. This must become
  per-proxy state (with the buffer reuse preserved some other way) *before*
  anything suspends; it is part of the first PR.
- `rv32::memory_slot_pool` (`rv32/threaded_machine.h:171`) is fine — acquire
  and release both operate on the current thread's pool, so a machine that
  migrates threads across a suspension just returns its buffer to a different
  free list. The header comment about destroying a slot on its constructing
  thread should be reworded.
- `make_hog_exec_context()`'s per-thread `RuntimeContext`
  (`precompiles/context.cc:42-48`, used at `executor_context.cc:155,250,305`
  and `rpc_from_view_gateway.cc:335`) is fine: `Rv32Context` holds no
  per-contract state and `NativeContext` holds a const map.
- `StorageSnapshotCache` is already `SynchronizedMap`-backed and documented as
  shared between concurrent transactions.

## Server side: already async

`HogDbExecutorServer` is already an `x23` async `RpcServer`
(`hogd/hog_db/rpc_from_exec.h:74`, `hogd/base/rpc_server.h:27`
`requires x23::implements_async_rpc`). Its methods return plain values, so each
call still runs to completion on the serving coroutine — but nothing on the
hog_db side has to change for hog_exec to switch clients. `EventServerProxy`
(`runtime/event_server_proxy.h:42`) is already an async client used from
hog_exec, so the pattern exists in-tree.

Note that `SRpcMulticlient` gets its parallelism from one connection per LLC
(`x23::PerCpuSet`), and a synchronous-bodied async server serializes requests
per connection. Keep the multiclient fan-out when swapping to
`x23::RpcMulticlient`; dropping to a single connection would serialize every DB
read in the process.

## Staging

1. (this PR) Move `current_subtransaction_batch_` off `thread_local`, and
   coroutinize the syscall seam: `syscall_entry` / `wrap_outer` return
   `MaybeAsyncSyscallResult`, `wrap_after_parse` becomes an
   `x23::Async<SyscallResult>` coroutine owning the parsed args and the alias
   guard. No inner syscall changes; all three consumers block on the result
   with `operator()()`. Behavior-identical.
2. Coroutinize the spine upward: `Runtime::invoke` → `Async<InvokeResult>`,
   `SystemCallHandler::{invoke, root_invoke}`, `NativeContract::function_t`,
   `run_propose_transaction` / `run_validate_transaction` /
   `run_view_transaction`. Still blocking at the leaf.
3. Make the storage read path `MaybeAsync`: `StorageSnapshotCache::try_get_*`
   (value on hit, `Async` on miss), `ItemView::load`, `DatabaseProxy`, and the
   DB-touching inner syscalls. Now suspension is real.
4. Swap `DatabaseProxy`'s client to `x23::RpcMulticlient`
   (`item/database_proxy.h:100-101`).
5. Delete `DispatchPool` and the `worker_pool_.submit` wrappers in
   `ExecutorContext` and `ViewGatewayRpc`; retune `-nthreads` onto the x23
   `ThreadPool`.

## Risk to watch at the last step

Contract execution is CPU-bound and today it is confined to the DispatchPool,
leaving the CoroPoll/ThreadPool workers free for I/O. After that step a
long-running RV32 invocation occupies an x23 `ThreadPool` worker until it
suspends. Gas bounds the work, but the pool has to be sized for it, and
anything that must stay responsive (shutdown, heartbeats, the coordinator RPC
loop) must not end up behind a queue of executions. This is the step most
likely to turn the cleanup into a latency regression, and it is worth a
loadgen comparison before and after rather than a code review.

Determinism is unaffected throughout: each transaction stays a strictly
sequential coroutine, and cross-transaction interleaving on a thread only
touches state that is already shared-safe (once step 0 lands).
