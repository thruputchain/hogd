// Experiment 1: Canary-instrumented Detached coroutine stress test.
//
// Creates many Detached coroutines with known canary values as parameters,
// suspends via pool.bg(), then verifies the canary values are intact after
// resume.  If the coroutine frame is being corrupted by a suspend-point
// index race (or any other frame corruption), the canary values near the
// index will be wrong.
//
// Additional probes:
//   - Multiple suspend points per coroutine (canaries checked after each)
//   - Canary values placed as both early and late parameters
//   - High thread count to maximize scheduling interleaving
//   - Long duration for soak testing

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <x23/async.h>
#include <x23/refcnt.h>
#include <x23/threadpool.h>

using namespace x23;
using namespace std::chrono;

static std::atomic<uint64_t> total_checks{0};
static std::atomic<uint64_t> total_coroutines{0};

// Canary pattern: 8 distinct values to surround the suspend-point index
// in the coroutine frame from different parameter positions.
static constexpr uint64_t kC1 = 0xDEADBEEFCAFEBABE;
static constexpr uint64_t kC2 = 0x0123456789ABCDEF;
static constexpr uint64_t kC3 = 0xFEDCBA9876543210;
static constexpr uint64_t kC4 = 0xA5A5A5A5A5A5A5A5;
static constexpr uint64_t kC5 = 0x5A5A5A5A5A5A5A5A;
static constexpr uint64_t kC6 = 0x1111111111111111;
static constexpr uint64_t kC7 = 0x2222222222222222;
static constexpr uint64_t kC8 = 0x3333333333333333;

static void
check_canaries(uint64_t c1, uint64_t c2, uint64_t c3, uint64_t c4, uint64_t c5,
               uint64_t c6, uint64_t c7, uint64_t c8, const char *where)
{
  if (c1 != kC1 || c2 != kC2 || c3 != kC3 || c4 != kC4 || c5 != kC5 ||
      c6 != kC6 || c7 != kC7 || c8 != kC8) {
    std::println(stderr,
                 "CORRUPTION DETECTED at {}!\n"
                 "  c1: {:016x} (expected {:016x}) {}\n"
                 "  c2: {:016x} (expected {:016x}) {}\n"
                 "  c3: {:016x} (expected {:016x}) {}\n"
                 "  c4: {:016x} (expected {:016x}) {}\n"
                 "  c5: {:016x} (expected {:016x}) {}\n"
                 "  c6: {:016x} (expected {:016x}) {}\n"
                 "  c7: {:016x} (expected {:016x}) {}\n"
                 "  c8: {:016x} (expected {:016x}) {}",
                 where, c1, kC1, c1 != kC1 ? "WRONG" : "ok", c2, kC2,
                 c2 != kC2 ? "WRONG" : "ok", c3, kC3,
                 c3 != kC3 ? "WRONG" : "ok", c4, kC4,
                 c4 != kC4 ? "WRONG" : "ok", c5, kC5,
                 c5 != kC5 ? "WRONG" : "ok", c6, kC6,
                 c6 != kC6 ? "WRONG" : "ok", c7, kC7,
                 c7 != kC7 ? "WRONG" : "ok", c8, kC8,
                 c8 != kC8 ? "WRONG" : "ok");
    abort();
  }
  total_checks.fetch_add(1, std::memory_order_relaxed);
}

// Variant A: Single suspend point, 8 canary parameters.
// The canaries surround whatever the compiler puts in the frame.
Detached
canary_single(RefPtr<DestroyInhibit> di, ThreadPool &pool,
              std::atomic<bool> &stop, uint64_t c1, uint64_t c2, uint64_t c3,
              uint64_t c4, uint64_t c5, uint64_t c6, uint64_t c7, uint64_t c8)
{
  while (!stop.load(std::memory_order_relaxed)) {
    // Check before suspend
    check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "before bg()");

    co_await pool.bg();

    // Check after resume — if frame was corrupted during suspend,
    // these will be wrong
    check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "after bg()");
  }
  total_coroutines.fetch_add(1, std::memory_order_relaxed);
}

// Variant B: Multiple suspend points. The suspend-point index changes
// at each co_await, so corruption is more likely to hit different
// locations in the frame.
Detached
canary_multi(RefPtr<DestroyInhibit> di, ThreadPool &pool,
             std::atomic<bool> &stop, uint64_t c1, uint64_t c2, uint64_t c3,
             uint64_t c4, uint64_t c5, uint64_t c6, uint64_t c7, uint64_t c8)
{
  while (!stop.load(std::memory_order_relaxed)) {
    check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "multi: before bg1");
    co_await pool.bg();
    check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "multi: after bg1");

    co_await pool.bg();
    check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "multi: after bg2");

    co_await pool.bg();
    check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "multi: after bg3");
  }
  total_coroutines.fetch_add(1, std::memory_order_relaxed);
}

// Variant C: Detached coroutine spawned from a lambda (like rpc.cc:167).
// This matches the exact pattern that TSAN warns about.
void
canary_lambda_spawn(RefPtr<DestroyInhibit> di, ThreadPool &pool,
                    std::atomic<bool> &stop)
{
  [](RefPtr<DestroyInhibit> di, ThreadPool &pool, std::atomic<bool> &stop,
     uint64_t c1, uint64_t c2, uint64_t c3, uint64_t c4, uint64_t c5,
     uint64_t c6, uint64_t c7, uint64_t c8) -> Detached {
    while (!stop.load(std::memory_order_relaxed)) {
      check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "lambda: before bg");
      co_await pool.bg();
      check_canaries(c1, c2, c3, c4, c5, c6, c7, c8, "lambda: after bg");
    }
    total_coroutines.fetch_add(1, std::memory_order_relaxed);
  }(di, pool, stop, kC1, kC2, kC3, kC4, kC5, kC6, kC7, kC8);
}

// Variant D: Fan-out and gather pattern (like FanoutServer).
// Create K Async<void> coroutines with canaries, await all.
Detached
canary_fanout(RefPtr<DestroyInhibit> di, ThreadPool &pool,
              std::atomic<bool> &stop, uint64_t c1, uint64_t c2)
{
  while (!stop.load(std::memory_order_relaxed)) {
    size_t K = std::max(size_t(2), pool.nthreads_);
    std::vector<Async<void>> handles;
    handles.reserve(K);
    for (size_t i = 0; i < K; i++) {
      handles.push_back([](ThreadPool &pool, uint64_t ca,
                           uint64_t cb) -> Async<void> {
        if (ca != kC1 || cb != kC2) {
          std::println(stderr, "CORRUPTION in fanout child before suspend!");
          abort();
        }
        co_await pool.bg();
        if (ca != kC1 || cb != kC2) {
          std::println(stderr, "CORRUPTION in fanout child after suspend!");
          abort();
        }
      }(pool, c1, c2));
    }
    for (auto &h : handles)
      co_await h;
    total_checks.fetch_add(K, std::memory_order_relaxed);
  }
  total_coroutines.fetch_add(1, std::memory_order_relaxed);
}

int
main(int argc, char **argv)
{
  unsigned threads = std::max(2U, unsigned(nproc()));
  unsigned seconds = 30;

  int opt;
  while ((opt = getopt(argc, argv, "t:d:")) != -1) {
    switch (opt) {
    case 't':
      threads = unsigned(atoi(optarg));
      break;
    case 'd':
      seconds = unsigned(atoi(optarg));
      break;
    default:
      std::println(stderr, "usage: {} [-t threads] [-d seconds]", argv[0]);
      return 2;
    }
  }

  std::println("=== Canary coroutine frame integrity test ===");
  std::println("threads={}, duration={}s", threads, seconds);

  ThreadPool pool(threads);
  std::atomic<bool> stop{false};
  DestroyInhibit di;

  // Spawn many coroutines of each variant
  unsigned ncoros = threads * 4;
  std::println("Spawning {} coroutines of each variant ({} total)...", ncoros,
               ncoros * 4);

  for (unsigned i = 0; i < ncoros; i++) {
    // Variant A: single suspend
    canary_single(di, pool, stop, kC1, kC2, kC3, kC4, kC5, kC6, kC7, kC8);

    // Variant B: multi suspend
    canary_multi(di, pool, stop, kC1, kC2, kC3, kC4, kC5, kC6, kC7, kC8);

    // Variant C: lambda-spawned (like rpc.cc dispatch)
    canary_lambda_spawn(di, pool, stop);

    // Variant D: fan-out (like FanoutServer)
    canary_fanout(di, pool, stop, kC1, kC2);
  }

  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);

  while (steady_clock::now() < deadline) {
    poll(nullptr, 0, 1000);
    auto elapsed = duration<double>(steady_clock::now() - start).count();
    std::println("  [{:.1f}s] checks: {}, coroutines completed: {}", elapsed,
                 total_checks.load(std::memory_order_relaxed),
                 total_coroutines.load(std::memory_order_relaxed));
  }

  stop.store(true, std::memory_order_relaxed);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start);
  std::println("\n=== PASS: No corruption detected ===");
  std::println("Total checks: {}", total_checks.load());
  std::println("Total coroutines completed: {}", total_coroutines.load());
  std::println("Duration: {:.2f}s", elapsed.count());
  return 0;
}
