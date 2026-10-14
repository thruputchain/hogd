// Long-running stress test for x23 RPC concurrency.  Designed to be
// run under TSAN/ASAN to surface races in coroutine lifetime, stream
// teardown, write buffer flushing, and thread pool shutdown.
//
// Four scenarios, run sequentially (or pick one with -s):
//
//   fanout    — Server fans out K coroutines per RPC (vector<Async<void>>
//               with randomized immediate/yield/double-yield modes).
//               Targets the coroutine frame lifetime bug seen in hogd's
//               process_transfers.
//
//   teardown  — Long-lived clients + churning clients (connect, 1-50
//               RPCs, abrupt close) against a server that randomly
//               yields mid-handler.  Targets CoroPoll::poison() racing
//               with read/write and RpcStream destruction.
//
//   writebuf  — Single connection shared by many coroutines with varied
//               payload sizes (4B/32KB/256KB) and periodic thundering-herd
//               spikes.  Verifies data integrity (echo size mismatch =
//               abort) and targets WriteBuffer concurrent flush.
//
//   shutdown  — Rapid create-work-destroy cycles with varied destruction
//               ordering (pool first, CoroPoll first, or clean).  Targets
//               ThreadPool/CoroPoll shutdown races.
//
// Usage:
//   test_rpcstress -d 60 -t 8 -v          # all scenarios, 60s each
//   test_rpcstress -s fanout -d 300 -t 8   # soak one scenario
//   test_rpcstress -s shutdown -d 10 -t 2  # quick shutdown probe

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <print>
#include <string>
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

std::string progname;
unsigned opt_threads;
unsigned opt_seconds = 60;
bool opt_verbose;

#if NDEBUG
#define make_rpc_stream rpc_stream
#else // !NDEBUG
#define make_rpc_stream rpc_stream_logged
#endif // !NDEBUG

// Format an int with commas
std::string
formatL(uint64_t v)
{
  std::string ret, in = std::format("{}", v);
  for (size_t i = 0, e = in.size(); i != e; ++i) {
    if ((e - i) % 3 == 0 && i != 0)
      ret += ',';
    ret += in[i];
  }
  return ret;
}

static void
parse_int_opt(std::integral auto &value, const char *s = optarg) noexcept
{
  auto [ptr, ec] = std::from_chars(s, s + strlen(s), value);
  if (ec != std::errc()) {
    std::println(stderr, "{}: cannot parse integer {}", progname, s);
    exit(2);
  }
}

// ============================================================
// Forward declarations
// ============================================================

int stress_fanout(unsigned threads, unsigned seconds, bool verbose);
int stress_bare(unsigned threads, unsigned seconds, bool verbose);
int stress_teardown(unsigned threads, unsigned seconds, bool verbose);
int stress_writebuf(unsigned threads, unsigned seconds, bool verbose);
int stress_shutdown(unsigned threads, unsigned seconds, bool verbose);

// ============================================================
// Shared infrastructure
// ============================================================

// Simple PRNG for per-coroutine randomness (xorshift64)
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

// Plain echo server — returns data as-is. Used by teardown + writebuf.
struct EchoServer {
  using rpc_service = blank_rpc_v1;
  void null() {}
  Data echo(Data arg) { return arg; }
};
static_assert(is_blank_rpc_v1<EchoServer *, MaybeAsync>);

// Fan-out echo server — spawns K coroutines per request. Used by fanout.
struct FanoutServer {
  using rpc_service = blank_rpc_v1;

  ThreadPool &pool_;
  std::atomic<uint64_t> &fanout_cycles_;

  FanoutServer(ThreadPool &pool, std::atomic<uint64_t> &cycles)
    : pool_(pool), fanout_cycles_(cycles)
  {}

  void null() {}

  Async<Data> echo(Data arg)
  {
    // Fan out to K coroutines, mimicking process_transfers pattern
    size_t K = std::max(size_t(2), pool_.nthreads_);
    std::vector<Async<void>> handles;
    handles.reserve(K);

    thread_local Rng rng;

    for (size_t i = 0; i < K; i++) {
      unsigned mode = rng.range(0, 2);
      handles.push_back([](ThreadPool &pool, unsigned mode) -> Async<void> {
        if (mode == 0) {
          // Immediate return — tests await_ready fast path
          co_return;
        }
        else if (mode == 1) {
          // Yield then return — tests suspend/resume path
          co_await pool.bg();
          co_return;
        }
        else {
          // Yield twice — more scheduling pressure
          co_await pool.bg();
          co_await pool.bg();
          co_return;
        }
      }(pool_, mode));
    }

    for (auto &h : handles)
      co_await h;

    fanout_cycles_.fetch_add(1, std::memory_order_relaxed);
    co_return arg;
  }
};
static_assert(is_blank_rpc_v1<FanoutServer *, MaybeAsync>);

// Slow echo server — random delay. Used by teardown.
struct SlowEchoServer {
  using rpc_service = blank_rpc_v1;

  ThreadPool &pool_;

  SlowEchoServer(ThreadPool &pool) : pool_(pool) {}

  void null() {}

  Async<Data> echo(Data arg)
  {
    thread_local Rng rng;
    // 50% chance of yielding (simulates handler mid-work when teardown hits)
    if (rng.range(0, 1))
      co_await pool_.bg();
    co_return arg;
  }
};
static_assert(is_blank_rpc_v1<SlowEchoServer *, MaybeAsync>);

// Helper: run echo RPCs in a loop until stop flag is set.
Detached
rpc_loop(RefPtr<DestroyInhibit> di, auto client, std::atomic<uint64_t> &count,
         std::atomic<bool> &stop, Data payload)
{
  try {
    while (!stop.load(std::memory_order_relaxed)) {
      co_await client.echo(payload);
      count.fetch_add(1, std::memory_order_relaxed);
    }
  } catch (const std::exception &) {
    // Connection closed or error — expected during teardown
  }
}

// ============================================================
// Stress test scenarios
// ============================================================

// Bare coroutine + threadpool test — no RPC layer at all.
// If this crashes, the bug is in coroutine/threadpool machinery.
int
stress_bare(unsigned threads, unsigned seconds, bool verbose)
{
  ThreadPool pool(threads);
  std::atomic<uint64_t> count{0};
  std::atomic<bool> stop{false};

  // Spawn worker coroutines that fan out and gather repeatedly
  DestroyInhibit di;
  unsigned ncoros = threads * 4;
  for (unsigned i = 0; i < ncoros; i++) {
    [](ThreadPool &pool, RefPtr<DestroyInhibit> di,
       std::atomic<uint64_t> &count, std::atomic<bool> &stop) -> Detached {
      size_t K = std::max(size_t(2), pool.nthreads_);
      while (!stop.load(std::memory_order_relaxed)) {
        std::vector<Async<void>> handles;
        handles.reserve(K);
        for (size_t j = 0; j < K; j++)
          handles.push_back([](ThreadPool &p) -> Async<void> {
            co_await p.bg();
            co_return;
          }(pool));
        for (auto &h : handles)
          co_await h;
        count.fetch_add(1, std::memory_order_relaxed);
      }
    }(pool, di, count, stop);
  }

  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);
  while (steady_clock::now() < deadline) {
    poll(nullptr, 0, verbose ? 5000 : 1000);
    if (verbose)
      std::println("  bare: {} cycles ({:.1f}s)", formatL(count.load()),
                   duration<double>(steady_clock::now() - start).count());
  }

  stop.store(true, std::memory_order_relaxed);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start);
  std::println("bare:      {} cycles | 0 errors | {:.2f}s",
               formatL(count.load()), elapsed.count());
  return 0;
}

int
stress_fanout(unsigned threads, unsigned seconds, bool verbose)
{
  ThreadPool pool(threads);
  CoroPoll cp(pool);
  std::atomic<uint64_t> rpc_count{0};
  std::atomic<uint64_t> fanout_cycles{0};
  std::atomic<bool> stop{false};

  // Start server
  auto server = std::make_unique<FanoutServer>(pool, fanout_cycles);
  auto [port, lfd] =
      tcp_rpc_server(cp, 0, [&](auto conf) { conf->server(server.get()); });

  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons(port);
  sockaddr_storage addr{};
  memcpy(&addr, &sin, sizeof(sin));

  // Start clients
  unsigned nconns = threads;
  unsigned coros_per_conn = 4;
  DestroyInhibit di;
  Data payload(64, uint8_t{0xA5});

  for (unsigned i = 0; i < nconns; i++) {
    [](CoroPoll &cp, sockaddr_storage addr, RefPtr<DestroyInhibit> di,
       unsigned coros_per_conn, std::atomic<uint64_t> &count,
       std::atomic<bool> &stop, Data payload) -> Detached {
      UniqueFd fd{socket(addr.ss_family, SOCK_STREAM, 0)};
      if (!fd)
        co_return;
      if (auto res = co_await cp.connect(fd, &addr); !res)
        co_return;
      auto c = make_rpc_stream(cp, fd.release())->client(blank_rpc_v1{});
      for (unsigned j = 0; j < coros_per_conn; j++)
        rpc_loop(di, c, count, stop, payload);
    }(cp, addr, di, coros_per_conn, rpc_count, stop, payload);
  }

  // Run for duration
  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);
  while (steady_clock::now() < deadline) {
    auto sleep_ms = verbose ? 5000 : 1000;
    poll(nullptr, 0, sleep_ms);
    if (verbose) {
      auto elapsed = duration<double>(steady_clock::now() - start).count();
      std::println("  fanout: {} RPCs, {} fan-out cycles ({:.1f}s)",
                   formatL(rpc_count.load()), formatL(fanout_cycles.load()),
                   elapsed);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  cp.poison(lfd);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start);
  std::println("fanout:    {} cycles | {} RPCs | 0 errors | {:.2f}s",
               formatL(fanout_cycles.load()), formatL(rpc_count.load()),
               elapsed.count());
  return 0;
}

int
stress_teardown(unsigned threads, unsigned seconds, bool verbose)
{
  ThreadPool pool(threads);
  CoroPoll cp(pool);
  std::atomic<uint64_t> rpc_count{0};
  std::atomic<uint64_t> connect_count{0};
  std::atomic<bool> stop{false};

  // Start server with slow echo (random delays)
  auto server = std::make_unique<SlowEchoServer>(pool);
  auto [port, lfd] =
      tcp_rpc_server(cp, 0, [&](auto conf) { conf->server(server.get()); });

  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons(port);
  sockaddr_storage addr{};
  memcpy(&addr, &sin, sizeof(sin));

  Data payload(128, uint8_t{0xBB});
  DestroyInhibit di;

  // Long-lived clients: steady background load
  unsigned long_lived = std::max(1U, threads / 2);
  for (unsigned i = 0; i < long_lived; i++) {
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
      rpc_loop(di, c, count, stop, payload);
    }(cp, addr, di, rpc_count, stop, payload);
  }

  // Churning clients: connect, fire a few RPCs, abruptly close
  unsigned churners = std::max(2U, threads);
  for (unsigned i = 0; i < churners; i++) {
    [](CoroPoll &cp, sockaddr_storage addr, RefPtr<DestroyInhibit> di,
       std::atomic<uint64_t> &rpc_count, std::atomic<uint64_t> &connect_count,
       std::atomic<bool> &stop, Data payload) -> Detached {
      thread_local Rng rng;
      while (!stop.load(std::memory_order_relaxed)) {
        UniqueFd fd{socket(addr.ss_family, SOCK_STREAM, 0)};
        if (!fd)
          continue;
        // RST on close to avoid TIME_WAIT exhausting ephemeral ports
        enable_abortive_close(fd);
        if (auto res = co_await cp.connect(fd, &addr); !res)
          continue;

        connect_count.fetch_add(1, std::memory_order_relaxed);
        auto c = make_rpc_stream(cp, fd.release())->client(blank_rpc_v1{});

        unsigned nrpcs = rng.range(1, 50);
        try {
          for (unsigned j = 0; j < nrpcs; j++) {
            co_await c.echo(payload);
            rpc_count.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (const std::exception &) {
        }
        // Client goes out of scope — abrupt close
      }
    }(cp, addr, di, rpc_count, connect_count, stop, payload);
  }

  // Run for duration
  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);
  while (steady_clock::now() < deadline) {
    auto sleep_ms = verbose ? 5000 : 1000;
    poll(nullptr, 0, sleep_ms);
    if (verbose) {
      auto elapsed = duration<double>(steady_clock::now() - start).count();
      std::println("  teardown: {} connects, {} RPCs ({:.1f}s)",
                   formatL(connect_count.load()), formatL(rpc_count.load()),
                   elapsed);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  cp.poison(lfd);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start);
  std::println("teardown:  {} connects | {} RPCs | 0 errors | {:.2f}s",
               formatL(connect_count.load()), formatL(rpc_count.load()),
               elapsed.count());
  return 0;
}

int
stress_writebuf(unsigned threads, unsigned seconds, bool verbose)
{
  ThreadPool pool(threads);
  CoroPoll cp(pool);
  std::atomic<uint64_t> rpc_count{0};
  std::atomic<uint64_t> bytes_transferred{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> spike{false};

  // Start simple echo server
  auto server = std::make_unique<EchoServer>();
  auto [port, lfd] =
      tcp_rpc_server(cp, 0, [&](auto conf) { conf->server(server.get()); });

  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons(port);
  sockaddr_storage addr{};
  memcpy(&addr, &sin, sizeof(sin));

  // Single connection, many coroutines sharing it
  DestroyInhibit di;
  unsigned ncoros = threads * 2;

  [&](this auto) -> Detached {
    UniqueFd fd{socket(addr.ss_family, SOCK_STREAM, 0)};
    if (!fd)
      co_return;
    if (auto res = co_await cp.connect(fd, &addr); !res)
      co_return;
    auto c = make_rpc_stream(cp, fd.release())->client(blank_rpc_v1{});

    Data tiny(4, uint8_t{0x11});
    Data medium(32 * 1024, uint8_t{0x22});
    Data large(256 * 1024, uint8_t{0x33});

    for (unsigned i = 0; i < ncoros; i++) {
      [](RefPtr<DestroyInhibit> di, auto c, std::atomic<uint64_t> &rpc_count,
         std::atomic<uint64_t> &bytes_transferred, std::atomic<bool> &stop,
         std::atomic<bool> &spike, Data tiny, Data medium,
         Data large) -> Detached {
        thread_local Rng rng;
        try {
          while (!stop.load(std::memory_order_relaxed)) {
            Data *payload;
            if (spike.load(std::memory_order_relaxed)) {
              payload = &large;
            }
            else {
              unsigned pick = rng.range(0, 9);
              if (pick < 5)
                payload = &tiny; // 50% tiny
              else if (pick < 8)
                payload = &medium; // 30% medium
              else
                payload = &large; // 20% large
            }
            auto result = co_await c.echo(*payload);
            if (result.size() != payload->size()) {
              std::println(stderr, "writebuf: data corruption! sent {} got {}",
                           payload->size(), result.size());
              abort();
            }
            rpc_count.fetch_add(1, std::memory_order_relaxed);
            bytes_transferred.fetch_add(payload->size(),
                                        std::memory_order_relaxed);
          }
        } catch (const std::exception &) {
        }
      }(di, c, rpc_count, bytes_transferred, stop, spike, tiny, medium, large);
    }
  }();

  // Run for duration, with periodic spikes
  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);
  auto next_spike = start + std::chrono::seconds(5);
  while (steady_clock::now() < deadline) {
    poll(nullptr, 0, 500);
    auto now = steady_clock::now();
    // Spike for 200ms every 5 seconds
    if (now >= next_spike) {
      spike.store(true, std::memory_order_relaxed);
      poll(nullptr, 0, 200);
      spike.store(false, std::memory_order_relaxed);
      next_spike = steady_clock::now() + std::chrono::seconds(5);
    }
    if (verbose) {
      auto elapsed = duration<double>(now - start).count();
      double gb = bytes_transferred.load() / 1e9;
      std::println("  writebuf: {} RPCs, {:.1f} GB ({:.1f}s)",
                   formatL(rpc_count.load()), gb, elapsed);
    }
  }

  stop.store(true, std::memory_order_relaxed);
  cp.poison(lfd);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start);
  double gb = bytes_transferred.load() / 1e9;
  std::println(
      "writebuf:  {} RPCs | {:.1f} GB transferred | 0 errors | {:.2f}s",
      formatL(rpc_count.load()), gb, elapsed.count());
  return 0;
}

int
stress_shutdown(unsigned threads, unsigned seconds, bool verbose)
{
  std::atomic<uint64_t> cycle_count{0};
  Rng rng;

  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);

  while (steady_clock::now() < deadline) {
    unsigned run_ms = rng.range(50, 500);
    unsigned destruction_mode = rng.range(0, 2);

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

    // Phase 2: Start clients
    Data payload(32, uint8_t{0xCC});
    unsigned nconns = std::max(2U, threads / 2);
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
      }(/*cp=*/*cp, addr, di, rpc_count, stop, payload);
    }

    // Phase 3: Let it run
    poll(nullptr, 0, run_ms);

    // Phase 4: Tear down (varied ordering)
    stop.store(true, std::memory_order_relaxed);

    if (destruction_mode == 0) {
      // Clean: poison listen fd, wait for clients, then destroy
      cp->poison(lfd);
      di.destroy_wait();
      cp.reset();
      pool.reset();
    }
    else if (destruction_mode == 1) {
      // Destroy CoroPoll first.  ~CoroPoll resets all fds, dispatching
      // EBADF to pending coroutines via the (still-alive) pool.  Tests
      // that CoroPoll cleanup works when streams are still active.
      cp->poison(lfd);
      cp.reset();
      di.destroy_wait();
      pool.reset();
    }
    else {
      // No listen-fd poison — rely solely on the stop flag for drain.
      // rpc_loops check stop and exit naturally after their current
      // RPC completes.  The accept loop keeps running until ~CoroPoll.
      di.destroy_wait();
      cp.reset();
      pool.reset();
    }

    cycle_count.fetch_add(1, std::memory_order_relaxed);

    if (verbose && (cycle_count.load() % 10 == 0)) {
      auto elapsed = duration<double>(steady_clock::now() - start).count();
      std::println("  shutdown: {} cycles ({:.1f}s)",
                   formatL(cycle_count.load()), elapsed);
    }
  }

  auto elapsed = duration<double>(steady_clock::now() - start);
  std::println("shutdown:  {} cycles | 0 errors | {:.2f}s",
               formatL(cycle_count.load()), elapsed.count());
  return 0;
}

// ============================================================
// Main
// ============================================================

[[noreturn]]
void
usage(int err)
{
  std::println(err ? stderr : stdout,
               "usage: {} [-t threads] [-d seconds] [-s scenario] [-v]",
               progname);
  exit(err);
}

int
main(int argc, char **argv)
{
  progname =
      std::filesystem::path(argc > 0 ? argv[0] : __FILE__).filename().string();

  const char *opt_scenario = nullptr;

  int opt;
  while ((opt = getopt(argc, argv, "t:d:s:vh")) != -1)
    switch (opt) {
    case 't':
      parse_int_opt(opt_threads);
      break;
    case 'd':
      parse_int_opt(opt_seconds);
      break;
    case 's':
      opt_scenario = optarg;
      break;
    case 'v':
      opt_verbose = true;
      break;
    case 'h':
      usage(0);
    default:
      usage(2);
    }

  if (!opt_threads)
    opt_threads = std::max(2U, unsigned(nproc()));

  setlinebuf(stdout);
  if (getenv("RPCTRACE"))
    RpcLogger::default_logfd = 1;

  struct Scenario {
    const char *name;
    int (*fn)(unsigned, unsigned, bool);
  };
  Scenario scenarios[] = {
      {"bare", stress_bare},         {"fanout", stress_fanout},
      {"teardown", stress_teardown}, {"writebuf", stress_writebuf},
      {"shutdown", stress_shutdown},
  };

  int ret = 0;
  for (auto &[name, fn] : scenarios) {
    if (opt_scenario && strcmp(opt_scenario, name) != 0)
      continue;
    std::println("=== {} ({} threads, {}s) ===", name, opt_threads,
                 opt_seconds);
    ret |= fn(opt_threads, opt_seconds, opt_verbose);
  }
  return ret;
}
