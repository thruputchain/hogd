// Reproducer for a clang 22 coroutine codegen regression that segfaults
// MaybeAsync-returning coroutines when their completed frame is destroyed.
// Verified: crashes with Homebrew clang 22.1.5, passes with clang 21.1.8
// (macOS 26.5.2 arm64, -O0).  Found via hogd#3557, the first change to ship
// suspending MaybeAsync syscall bodies; this test is expected to go red on
// toolchains carrying the bug.
//
// The pattern: a coroutine declared to return MaybeAsync<T> gets its promise
// from Async<T> via x23's coroutine_traits inheritance, so
// get_return_object() returns Async<T> and the ramp converts it to
// MaybeAsync<T> on return.  clang 22 clones into the coroutine's .destroy
// function a guarded cleanup for that return-object temporary:
//
//   - the guard flag lives in the coroutine frame (the ramp sets it and
//     never clears it), but
//   - the temporary lived on the *ramp's stack*, so in the destroy clone the
//     cleanup targets a fresh stack slot that nothing ever writes.
//
// Destroying the completed frame (~MaybeAsync -> coroutine_handle::destroy
// at the final-suspend index) reads the stale guard and runs ~Async on that
// uninitialized slot: __builtin_coro_destroy(<stack garbage>).  In the hogd
// binary the clang-22 .destroy clone of syscall_GetBlockHeight_inner
// dispatches on the suspend index at frame+0x94, reads the guard byte at
// frame+0x95 on the final-suspend path, and calls ~Async<SyscallInnerResult>
// on x29-0x88, which no instruction on that path initializes.  The clang-21
// clone contains no such call.  TSAN reproduces the identical SEGV with zero
// race reports: this is not a lifetime bug in the Awaitable protocol.
//
// ~Async no-ops on a null handle, so the latent double-destroy only faults
// when the destroying thread's stack is dirty at that depth.  Fresh
// std::thread stacks are zero pages -- which is how this hid from small unit
// drivers -- while long-lived ThreadPool workers guarantee garbage.  The
// completer below therefore poisons its stack with 0x51 before resuming,
// making the fault deterministic and self-identifying: the crash is
// __builtin_coro_destroy(0x5151515151515151).
//
// Sibling coro_return_object_destroy_standalone.cc reproduces the same bug
// with only standard headers, for reporting upstream.  Nearest known
// upstream report is llvm/llvm-project#56532 (return-object conversion
// timing); this destroy-clone variant appears unreported as of 2026-08-23.

#include <print>
#include <thread>
#include <utility>

#include <x23/async.h>

using namespace x23;

using Result = std::pair<int, long>;
using MA = MaybeAsync<Result>;

namespace {

Async<int>
leaf(Async<int> &pending)
{
  int v = co_await pending;
  co_return v;
}

// The coroutine under test: declared MaybeAsync<T>, promise from Async<T>.
MA
inner(Async<int> &pending)
{
  int v = co_await leaf(pending);
  co_return Result{v + 1, v + 2};
}

Async<int>
outer(Async<int> pending)
{
  MA ma = inner(pending);
  Result r = co_await ma; // ~MA destroys inner's completed frame on the
                          // resuming (completer) thread
  co_return r.first + static_cast<int>(r.second);
}

// Fill the completer thread's stack with non-null garbage so the destroy
// clone's uninitialized cleanup slot holds a recognizable poison "handle".
[[gnu::noinline]] void
dirty_stack()
{
  volatile unsigned long long buf[2048];
  for (auto &x : buf)
    x = 0x5151515151515151ULL;
}

} // namespace

int
main()
{
  for (int i = 0; i < 50; i++) {
    auto [async_in, cb] = make_async<int>();
    Async<int> out = outer(std::move(async_in));
    std::thread completer([cb] {
      dirty_stack();
      cb(41);
    });
    int result = out();
    completer.join();
    if (result != 85) {
      std::println("iter {}: wrong result {}", i, result);
      return 1;
    }
  }
  std::println("ok");
  return 0;
}
