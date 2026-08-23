# clang 22 coroutine `.destroy` miscompile — reproducers

Root cause of the macOS-only CI segfaults on hogd#3557 (the PR that makes
syscall bodies return a suspending `MaybeAsyncSyscallInnerResult`).

## The bug

For a coroutine whose **declared return type differs from
`get_return_object()`'s type** — x23's `MaybeAsync<T>` pattern, where
`std::coroutine_traits<MaybeAsync<T>>` inherits `Async<T>`'s promise, so
`get_return_object()` returns `Async<T>` and the ramp converts it to
`MaybeAsync<T>` on return — Homebrew clang 22.1.5 clones into the coroutine's
`.destroy` function a guarded cleanup for that return-object temporary:

- the **guard flag** lives in the coroutine frame (set to 1 by the ramp,
  never cleared), but
- the **object** was a ramp-*stack* temporary, so in the destroy clone the
  cleanup's slot is a fresh, never-written stack alloca.

When the completed coroutine's frame is destroyed (`~MaybeAsync` →
`coroutine_handle::destroy` at the final-suspend index), the clone reads the
stale guard, then runs `~Async` on the uninitialized slot →
`__builtin_coro_destroy(<stack garbage>)` → SIGSEGV. `~Async` no-ops on a
null handle, so the crash only manifests when the destroying thread's stack
is dirty at that depth — which is why it fires deterministically on x23
ThreadPool workers (deep, reused stacks) and hid in small unit drivers
(fresh zero-page thread stacks).

clang 21.1.8 emits no such cleanup in the `.destroy` clone. Not a data race
(TSAN reproduces the SEGV with zero warnings), not a stack-size issue.

Disassembly evidence (Debug build of `loadgen_tests`,
`syscall_GetBlockHeight_inner`): the clang-22 `.destroy` clone dispatches on
the suspend index at frame+0x94; on the final-suspend path it reads the guard
byte at frame+0x95 and calls `~Async<SyscallInnerResult>` on `x29-0x88`,
which nothing on that path ever writes. clang-21's clone contains no
`~Async<SyscallInnerResult>` call at all.

## Files

- `standalone.cc` — zero-dependency single file (~120 lines, std headers
  only). Suitable for an llvm-project issue.

      clang++ -std=c++20 -O0 standalone.cc -o repro && ./repro
      # clang 22.1.5: SIGSEGV, destroy(0x5151515151515151)
      # clang 21.1.8: prints "ok"

- `x23_repro.cc` — same shape expressed with the real `x23::Async` /
  `x23::MaybeAsync`, for the x23 maintainer.

      clang++ -std=gnu++23 -O0 -stdlib=libc++ \
        -I <hogd>/x23 -I <hogd>/build/x23 \
        x23_repro.cc <hogd>/build/x23/x23/libx23.a -o repro && ./repro

Both crash with the exact production backtrace:
`~MaybeAsync` → `inner (.destroy)` → `~Async`/`~Task` of the coroutine's own
return-object type → `__builtin_coro_destroy(poison)`.

The `dirty_stack()` + cross-thread completion in the drivers is only a
detonator: it makes the uninitialized cleanup slot hold non-null poison
(`0x5151…51`) instead of lucky zeroes, turning the latent double-destroy
into a deterministic fault with a self-identifying address.

Verified 2026-08-23 on macOS 26.5.2 arm64, Homebrew clang 22.1.5 vs 21.1.8.
No matching upstream issue found; nearest relative is
https://github.com/llvm/llvm-project/issues/56532 (return-object conversion
timing).
