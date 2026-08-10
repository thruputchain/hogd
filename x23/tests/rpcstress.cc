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
// Each scenario is bounded by a watchdog that reports and dumps x23 state if
// the scenario stops making progress, rather than leaving ctest to kill the
// process silently; see the Watchdog comment below.
//
// Usage:
//   test_rpcstress -d 60 -t 8 -v          # all scenarios, 60s each
//   test_rpcstress -s fanout -d 300 -t 8   # soak one scenario
//   test_rpcstress -s shutdown -d 10 -t 2  # quick shutdown probe
//   test_rpcstress -s teardown -d 300 -w 0 # soak with the watchdog off
//   test_rpcstress -s bare -d 1 -w 3 -H 60 # see what the watchdog reports

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>

#include <x23/async.h>
#include <x23/event.h>
#include <x23/refcnt.h>
#include <x23/rpc.h>
#include <x23/signal.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>

#include "tests/xdrtest.hh"

using namespace x23;
using namespace std::chrono;

std::string progname;
unsigned opt_threads;
unsigned opt_seconds = 60;
// Seconds a scenario may spend past its own duration before the watchdog
// calls it hung.  0 disables the watchdog.
unsigned opt_watchdog_grace = 20;
// Stall this many seconds on entering the shutdown phase.  Nothing in a real
// run sets this: it exists so the watchdog's own reporting path can be
// exercised on demand, which a real hang -- rare, and rare on CI machines
// specifically -- offers no way to do.
unsigned opt_hang_shutdown;
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
// Hang watchdog
// ============================================================

// A scenario that stops making progress is the failure this test exists to
// catch, and it is also the one it reports worst.  ctest kills the process
// with SIGKILL at its 60s TIMEOUT, so the job log keeps the scenario's header
// line and nothing after it: no summary, no state, no core.  That is the whole
// of what CI has ever said about issue #1334 --
//
//     The following tests FAILED:
//          14 - rpcstress_teardown (Timeout)
//
// -- which is why the bug outlived several months of hits.
//
// So bound each scenario here, inside the ctest timeout, and spend the margin
// on saying something.  When the bound passes, name the phase that did not
// finish and then hand SIGUSR1 to x23's own handlers: CoroPollImpl::debug_dump
// prints every live file descriptor with the coroutine parked on it, and
// ThreadPool::debug_dump prints the worker and queue state.  Between them they
// identify the stuck connection and what it is waiting for, which is exactly
// the evidence a hang here needs.  Then abort, so there is a core to attach a
// debugger to as well (CI collects those via scripts/save_core_dumps.sh).
//
// A watchdog that fires early costs nothing: the alternative outcome was a
// ctest timeout on the same run, so a false positive replaces a silent failure
// with a described one rather than creating a failure of its own.
class Watchdog {
  std::mutex mtx_;
  std::condition_variable cv_;
  const char *phase_ = "startup";
  bool finished_ = false;

  std::string name_;
  seconds budget_;
  steady_clock::time_point start_ = steady_clock::now();
  std::thread thread_;

  [[noreturn]] void fire(const char *phase) const
  {
    auto waited = duration<double>(steady_clock::now() - start_).count();
    std::println(stderr,
                 "{}: {}: WATCHDOG: phase '{}' still running {:.1f}s in, past "
                 "a {}s budget -- treating as hung",
                 progname, name_, phase, waited, budget_.count());
    std::println(stderr,
                 "{}: {}: dumping x23 fd/coroutine and thread pool state, "
                 "then aborting for a core dump",
                 progname, name_);
    std::fflush(nullptr);

    // Runs the debug_dump callbacks CoroPoll and ThreadPool register for
    // SIGUSR1.  Calling the handler directly rather than raise()ing keeps this
    // independent of the process signal mask; either way the dumps happen on
    // x23's signal service thread, so give them a moment to land before the
    // abort cuts them off.
    signal_handler(SIGUSR1);
    std::this_thread::sleep_for(seconds(2));
    std::fflush(nullptr);
    abort();
  }

public:
  Watchdog(std::string name, seconds budget)
    : name_(std::move(name)), budget_(budget)
  {
    if (budget_ <= seconds(0))
      return;
    thread_ = std::thread([this] {
      std::unique_lock lk(mtx_);
      if (cv_.wait_for(lk, budget_, [this] { return finished_; }))
        return;
      // Fire while still holding the lock.  A scenario that finishes at
      // exactly the budget must not be aborted: the destructor sets finished_
      // under this lock, so having observed it false here means the decision
      // to fire cannot be overtaken by one to stand down.
      fire(phase_);
    });
  }

  Watchdog(const Watchdog &) = delete;
  Watchdog &operator=(const Watchdog &) = delete;

  ~Watchdog()
  {
    {
      std::lock_guard _lk(mtx_);
      finished_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

  // Label what the scenario is doing, so a hang is attributed rather than just
  // reported.  Shutdown is the phase issue #1334 hangs in, and telling it apart
  // from a hang during the run is most of the diagnosis.
  void phase(const char *p)
  {
    std::lock_guard _lk(mtx_);
    phase_ = p;
  }
};

// The armed watchdog for the scenario now running, so scenarios can label
// their phase without every one of them taking a parameter for it.  Only the
// thread running the scenarios touches this; the watchdog thread reaches the
// phase through the Watchdog's own lock.
Watchdog *cur_watchdog;

void
watchdog_phase(const char *phase)
{
  if (cur_watchdog)
    cur_watchdog->phase(phase);
  if (opt_hang_shutdown && !strcmp(phase, "shutdown"))
    std::this_thread::sleep_for(seconds(opt_hang_shutdown));
}

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

  watchdog_phase("shutdown");
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

  watchdog_phase("shutdown");
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

  watchdog_phase("shutdown");
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

  watchdog_phase("shutdown");
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
    watchdog_phase("run");
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
    watchdog_phase("shutdown");
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
               "usage: {} [-t threads] [-d seconds] [-s scenario] "
               "[-w watchdog_grace_seconds] [-H hang_shutdown_seconds] [-v]",
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
  while ((opt = getopt(argc, argv, "t:d:s:w:H:vh")) != -1)
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
    case 'w':
      parse_int_opt(opt_watchdog_grace);
      break;
    case 'H':
      parse_int_opt(opt_hang_shutdown);
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

  // A scenario runs for opt_seconds and then shuts down, so anything past
  // opt_seconds plus the grace is not slow, it is stuck.  Armed per scenario
  // rather than per process, so the bound holds whether one scenario was
  // selected or all of them run back to back.
  seconds budget(opt_seconds + opt_watchdog_grace);
  if (!opt_watchdog_grace)
    budget = seconds(0);

  int ret = 0;
  for (auto &[name, fn] : scenarios) {
    if (opt_scenario && strcmp(opt_scenario, name) != 0)
      continue;
    std::println("=== {} ({} threads, {}s) ===", name, opt_threads,
                 opt_seconds);
    Watchdog wd(name, budget);
    cur_watchdog = &wd;
    Defer _disarm{[] { cur_watchdog = nullptr; }};
    wd.phase("run");
    ret |= fn(opt_threads, opt_seconds, opt_verbose);
  }
  return ret;
}
