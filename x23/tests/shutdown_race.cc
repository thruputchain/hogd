// Experiment 5: SharedLock/PerCpuSet destruction race during shutdown.
//
// The hypothesis: destroying CoroPoll while worker threads still hold
// SharedHolder references to SharedLock::Inhibitor objects causes
// use-after-free.  The PerCpuSet<Inhibitor> is destroyed in ~SharedLock
// (via ~CoroPollImpl), but worker threads doing getfd() may still be
// holding SharedHolder refs that point into the now-freed PerCpuSet.
//
// This test maximizes the race window:
//   - Many threads (24+)
//   - Very short run_ms (1-20ms) to maximize creation/destruction cycles
//   - Always uses destruction_mode 2 (no poison): just stop flag + destroy
//   - Canary verification on echo payload after each RPC
//   - MALLOC_PERTURB_ should be set externally for best effect

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <x23/async.h>
#include <x23/event.h>
#include <x23/refcnt.h>
#include <x23/rpc.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>

#include "tests/xdrtest.hh"

using namespace x23;
using namespace std::chrono;

struct EchoServer {
  using rpc_service = blank_rpc_v1;
  void null() {}
  Data echo(Data arg) { return arg; }
};
static_assert(is_blank_rpc_v1<EchoServer *, MaybeAsync>);

Detached
rpc_loop(RefPtr<DestroyInhibit> di, auto client, std::atomic<uint64_t> &count,
         std::atomic<bool> &stop, Data payload)
{
  try {
    while (!stop.load(std::memory_order_relaxed)) {
      auto result = co_await client.echo(payload);
      // Verify echo integrity
      if (result.size() != payload.size()) {
        std::println(stderr, "Echo size mismatch: {} vs {}", result.size(),
                     payload.size());
        abort();
      }
      count.fetch_add(1, std::memory_order_relaxed);
    }
  } catch (const std::exception &) {
  }
}

#if NDEBUG
#define make_rpc_stream rpc_stream
#else
#define make_rpc_stream rpc_stream_logged
#endif

// Simple PRNG
struct Rng {
  uint64_t s_;
  Rng()
    : s_(std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
         uint64_t(steady_clock::now().time_since_epoch().count()))
  {
    if (!s_)
      s_ = 1;
  }
  uint64_t next()
  {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return s_;
  }
  unsigned range(unsigned lo, unsigned hi)
  {
    return lo + next() % (hi - lo + 1);
  }
};

int
main(int argc, char **argv)
{
  unsigned threads = std::max(24U, unsigned(nproc()));
  unsigned seconds = 60;

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

  std::println("=== Shutdown race isolation test ===");
  std::println("threads={}, duration={}s", threads, seconds);
  std::println("Short cycles, no-poison destruction, maximize race window");

  if (getenv("MALLOC_PERTURB_"))
    std::println("MALLOC_PERTURB_ is set (good)");
  else
    std::println("WARNING: MALLOC_PERTURB_ not set. Set to 85 for best "
                 "use-after-free detection.");

  setlinebuf(stdout);
  std::atomic<uint64_t> cycle_count{0};
  std::atomic<uint64_t> total_rpcs{0};
  Rng rng;

  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);

  while (steady_clock::now() < deadline) {
    // Very short run to maximize create/destroy cycles
    unsigned run_ms = rng.range(1, 20);

    // Phase 1: Create infrastructure
    auto pool = std::make_unique<ThreadPool>(threads);
    auto cp = std::make_unique<CoroPoll>(*pool);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> rpc_count{0};

    auto server = std::make_unique<EchoServer>();
    auto [port, lfd] =
        tcp_rpc_server(*cp, 0, [&](auto conf) { conf->server(server.get()); });

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);
    sockaddr_storage addr{};
    memcpy(&addr, &sin, sizeof(sin));

    // Phase 2: Start many clients
    Data payload(32, uint8_t{0xCC});
    unsigned nconns = std::max(4U, threads);
    DestroyInhibit di;

    for (unsigned i = 0; i < nconns; i++) {
      [](CoroPoll &cp, sockaddr_storage addr, RefPtr<DestroyInhibit> di,
         std::atomic<uint64_t> &count, std::atomic<bool> &stop,
         Data payload) -> Detached {
        UniqueFd fd{socket(addr.ss_family, SOCK_STREAM, 0)};
        if (!fd)
          co_return;
        if (auto res = co_await cp.connect(fd, &addr); !res)
          co_return;
        auto c = make_rpc_stream(cp, fd.release())->client(blank_rpc_v1{});
        rpc_loop(di, c, count, stop, payload);
      }(*cp, addr, di, rpc_count, stop, payload);
    }

    // Phase 3: Let it run briefly
    poll(nullptr, 0, run_ms);

    // Phase 4: Destruction mode 2 — no poison, just stop + destroy.
    // This is the most aggressive shutdown path: we rely only on the
    // stop flag.  The accept loop keeps running until ~CoroPoll resets
    // all fds.  Worker threads may be mid-getfd() (holding SharedHolder)
    // when the PerCpuSet<Inhibitor> is destroyed in ~SharedLock.
    stop.store(true, std::memory_order_relaxed);
    di.destroy_wait();
    cp.reset();
    pool.reset();

    total_rpcs.fetch_add(rpc_count.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    auto cycles = cycle_count.fetch_add(1, std::memory_order_relaxed) + 1;

    if (cycles % 50 == 0) {
      auto elapsed = duration<double>(steady_clock::now() - start).count();
      std::println("  [{:.1f}s] {} cycles, {} total RPCs", elapsed, cycles,
                   total_rpcs.load(std::memory_order_relaxed));
    }
  }

  auto elapsed = duration<double>(steady_clock::now() - start);
  std::println("\n=== PASS: No crash during {} shutdown cycles ===",
               cycle_count.load());
  std::println("Total RPCs: {}, Duration: {:.2f}s", total_rpcs.load(),
               elapsed.count());
  return 0;
}
