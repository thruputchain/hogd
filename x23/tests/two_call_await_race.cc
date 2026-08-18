// Isolate the shape of hogd's MempoolCoordinatorRpc::commit_block: launch two
// RPCs on two *different* connections, then co_await them one after the other,
// while a chaos loop poisons one connection so an exception is delivered from
// one await while the other call is still in flight.
//
// That is the shape behind hogd ISSUES.md #6, where the mempool died on the RPC
// reader thread inside commit_block's cold (exception) path -- once with
// free(): invalid pointer on a code address, once with SIGSEGV in
// __cxa_begin_catch on a garbage _Unwind_Exception*.
//
// Two client shapes are exercised:
//
//   bare (-b):  co_await a; co_await b;
//               An exception from `a` unwinds past `b`, so ~Async destroys a
//               coroutine frame whose RPC is still in flight -- which async.h
//               documents as UB and ~Awaitable panics on when NDEBUG is off.
//
//   drained:    await both, keep the first exception, then rethrow.
//               The hogd fix.  Must never destroy a running Async.
//
// Build with assertions on (-UNDEBUG), or this can only catch the crash and not
// the invariant violation that leads to it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
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

namespace {

// Always yields, so every call is genuinely in flight across a suspension
// rather than taking the synchronous fast path.
struct YieldingServer {
  using rpc_service = blank_rpc_v1;

  ThreadPool &pool_;
  std::atomic<uint64_t> &served_;

  YieldingServer(ThreadPool &pool, std::atomic<uint64_t> &served)
    : pool_(pool), served_(served)
  {}

  void null() {}

  Async<Data> echo(Data arg)
  {
    co_await pool_.bg();
    served_.fetch_add(1, std::memory_order_relaxed);
    co_return arg;
  }
};
static_assert(is_blank_rpc_v1<YieldingServer *, MaybeAsync>);

struct Counters {
  std::atomic<uint64_t> rounds{0};
  std::atomic<uint64_t> exceptions{0};
  std::atomic<uint64_t> served{0};
};

#if NDEBUG
#define make_rpc_stream rpc_stream
#else
#define make_rpc_stream rpc_stream_logged
#endif

// One worker owns two connections and drives commit_block's shape until one of
// its streams is poisoned out from under it.
Detached
two_call_worker(RefPtr<DestroyInhibit> di, CoroPoll &cp, sockaddr_storage addr,
                Counters &c, std::atomic<bool> &stop, bool bare,
                std::atomic<int> *poison_fd)
{
  UniqueFd fd_a{socket(addr.ss_family, SOCK_STREAM, 0)};
  UniqueFd fd_b{socket(addr.ss_family, SOCK_STREAM, 0)};
  if (!fd_a || !fd_b)
    co_return;
  if (auto r = co_await cp.connect(fd_a, &addr); !r)
    co_return;
  if (auto r = co_await cp.connect(fd_b, &addr); !r)
    co_return;

  const int raw_a = fd_a.get();
  auto stream_a = make_rpc_stream(cp, fd_a.release());
  auto stream_b = make_rpc_stream(cp, fd_b.release());
  auto client_a = stream_a->client(blank_rpc_v1{});
  auto client_b = stream_b->client(blank_rpc_v1{});

  // Publish the *first-awaited* connection's fd. Chaos breaks that one, so the
  // exception is delivered from the first await while the second call is still
  // in flight -- the only ordering in which the pre-fix shape abandons a
  // sibling Async. (Breaking the second stream instead is harmless: the throw
  // arrives at the last await with no sibling left to abandon.)
  poison_fd->store(raw_a, std::memory_order_release);

  // `a` is small so its failure lands quickly; `b` carries a large payload so
  // it is still in flight when `a` throws. Same asymmetry as the real thing,
  // where the event commit returns long before the 200k-tx db commit.
  Data small(64, uint8_t{0x5A});
  Data big(256 * 1024, uint8_t{0xA5});

  try {
    while (!stop.load(std::memory_order_relaxed)) {
      Async<Data> handle_a = client_a.echo(small);
      Async<Data> handle_b = client_b.echo(big);

      if (bare) {
        // The pre-fix hogd shape. If `a` throws here, handle_b is destroyed
        // while its RPC is still in flight.
        Data ra = co_await handle_a;
        Data rb = co_await handle_b;
        if (ra.size() != small.size() || rb.size() != big.size())
          abort();
      }
      else {
        std::exception_ptr first;
        std::optional<Data> ra;
        std::optional<Data> rb;
        try {
          ra = co_await handle_a;
        } catch (...) {
          first = std::current_exception();
        }
        try {
          rb = co_await handle_b;
        } catch (...) {
          if (!first)
            first = std::current_exception();
        }
        if (first) {
          c.exceptions.fetch_add(1, std::memory_order_relaxed);
          std::rethrow_exception(first);
        }
        if (ra->size() != small.size() || rb->size() != big.size())
          abort();
      }
      c.rounds.fetch_add(1, std::memory_order_relaxed);
    }
  } catch (const std::exception &) {
    // Expected once chaos poisons a stream; the point is to survive it.
    if (bare)
      c.exceptions.fetch_add(1, std::memory_order_relaxed);
  }
  poison_fd->store(-1, std::memory_order_release);
}

} // namespace

int
main(int argc, char **argv)
{
  unsigned threads = std::max(4U, unsigned(nproc()));
  unsigned seconds = 20;
  unsigned slots = 16;
  unsigned chaos_ms = 25;
  bool bare = false;

  int opt;
  while ((opt = getopt(argc, argv, "t:d:n:c:b")) != -1) {
    switch (opt) {
    case 't':
      threads = unsigned(atoi(optarg));
      break;
    case 'd':
      seconds = unsigned(atoi(optarg));
      break;
    case 'n':
      slots = unsigned(atoi(optarg));
      break;
    case 'c':
      chaos_ms = unsigned(atoi(optarg));
      break;
    case 'b':
      bare = true;
      break;
    default:
      std::println(stderr,
                   "usage: {} [-t threads] [-d seconds] [-n slots] "
                   "[-c chaos_ms] [-b]",
                   argv[0]);
      return 2;
    }
  }

  std::println("=== two-call await race (commit_block shape) ===");
  std::println("threads={} duration={}s slots={} chaos={}ms shape={}", threads,
               seconds, slots, chaos_ms, bare ? "bare" : "drained");

  ThreadPool pool(threads);
  CoroPoll cp(pool);
  Counters c;
  std::atomic<bool> stop{false};

  auto server = std::make_unique<YieldingServer>(pool, c.served);
  auto [port, lfd] =
      tcp_rpc_server(cp, 0, [&](auto conf) { conf->server(server.get()); });

  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons(port);
  sockaddr_storage addr{};
  memcpy(&addr, &sin, sizeof(sin));

  DestroyInhibit di;
  std::vector<std::atomic<int>> poison_fds(slots);
  for (auto &f : poison_fds)
    f.store(-1, std::memory_order_relaxed);

  auto spawn = [&](unsigned slot) {
    two_call_worker(di, cp, addr, c, stop, bare, &poison_fds[slot]);
  };
  for (unsigned i = 0; i < slots; i++)
    spawn(i);

  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);
  unsigned next = 0;
  while (steady_clock::now() < deadline) {
    poll(nullptr, 0, int(chaos_ms));
    // Break one worker's second connection while its two calls are in flight,
    // then respawn that slot so the race keeps recurring.
    unsigned slot = next++ % slots;
    if (int fd = poison_fds[slot].load(std::memory_order_acquire); fd >= 0)
      cp.poison(BorrowedFd{fd});
    if (poison_fds[slot].load(std::memory_order_acquire) < 0)
      spawn(slot);
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto &f : poison_fds)
    if (int fd = f.load(std::memory_order_acquire); fd >= 0)
      cp.poison(BorrowedFd{fd});
  cp.poison(lfd);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start).count();
  std::println("rounds={} exceptions={} served={} in {:.1f}s", c.rounds.load(),
               c.exceptions.load(), c.served.load(), elapsed);
  std::println("=== survived (no panic, no crash) ===");
  return 0;
}
