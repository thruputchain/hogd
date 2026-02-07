// Experiment 4: Isolate the rpc.cc:167-169 Detached dispatch race.
//
// The TSAN race at rpc.cc:167-169 involves Detached coroutine creation
// in the RPC dispatch path.  When an RPC handler returns MaybeAsync
// (async path, i.e. await_ready() returns false), the reader coroutine
// spawns a Detached coroutine to co_await the result and write it back.
//
// This test hammers exactly that path: many concurrent RPC calls where
// the server handler always returns Async (never the sync fast path),
// forcing every dispatch to go through the Detached spawn.
//
// We add canary verification on the server side: the echo payload
// is checked for integrity after the async yield.

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

// Server that ALWAYS yields (forces the Detached dispatch path).
// Also verifies payload integrity after the yield.
struct AlwaysAsyncServer {
  using rpc_service = blank_rpc_v1;

  ThreadPool &pool_;
  std::atomic<uint64_t> &count_;
  std::atomic<uint64_t> &corruptions_;

  AlwaysAsyncServer(ThreadPool &pool, std::atomic<uint64_t> &count,
                    std::atomic<uint64_t> &corruptions)
    : pool_(pool), count_(count), corruptions_(corruptions)
  {}

  void null() {}

  // Always return Async (never MaybeAsync fast path).
  // This forces rpc.cc:167 to spawn a Detached coroutine every time.
  Async<Data> echo(Data arg)
  {
    // Verify payload before yield
    uint8_t expected = arg.empty() ? 0 : arg[0];
    size_t sz = arg.size();
    for (size_t i = 0; i < sz; i++) {
      if (arg[i] != expected) {
        std::println(stderr,
                     "CORRUPTION: payload[{}] = {:02x}, expected {:02x} "
                     "(size={})",
                     i, arg[i], expected, sz);
        corruptions_.fetch_add(1, std::memory_order_relaxed);
        abort();
      }
    }

    // Always yield — this is the key: we want every RPC to go through
    // the Detached path at rpc.cc:167
    co_await pool_.bg();

    // Verify payload after yield — if the coroutine frame was
    // corrupted during suspend, this data may be wrong
    for (size_t i = 0; i < sz; i++) {
      if (arg[i] != expected) {
        std::println(stderr,
                     "CORRUPTION after yield: payload[{}] = {:02x}, "
                     "expected {:02x} (size={})",
                     i, arg[i], expected, sz);
        corruptions_.fetch_add(1, std::memory_order_relaxed);
        abort();
      }
    }

    count_.fetch_add(1, std::memory_order_relaxed);
    co_return arg;
  }
};
static_assert(is_blank_rpc_v1<AlwaysAsyncServer *, MaybeAsync>);

// Client RPC loop (fire-and-forget)
Detached
rpc_loop(RefPtr<DestroyInhibit> di, auto client, std::atomic<uint64_t> &count,
         std::atomic<bool> &stop, Data payload)
{
  try {
    while (!stop.load(std::memory_order_relaxed)) {
      auto result = co_await client.echo(payload);
      // Verify echo came back intact
      if (result.size() != payload.size()) {
        std::println(stderr, "Echo size mismatch: sent {} got {}",
                     payload.size(), result.size());
        abort();
      }
      for (size_t i = 0; i < result.size(); i++) {
        if (result[i] != payload[i]) {
          std::println(stderr,
                       "Echo data mismatch at [{}]: sent {:02x} got {:02x}", i,
                       payload[i], result[i]);
          abort();
        }
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

  std::println("=== RPC dispatch race isolation test ===");
  std::println("threads={}, duration={}s", threads, seconds);
  std::println("Every RPC forces the Detached dispatch path (rpc.cc:167)");

  ThreadPool pool(threads);
  CoroPoll cp(pool);
  std::atomic<uint64_t> client_count{0};
  std::atomic<uint64_t> server_count{0};
  std::atomic<uint64_t> corruptions{0};
  std::atomic<bool> stop{false};

  auto server =
      std::make_unique<AlwaysAsyncServer>(pool, server_count, corruptions);
  auto [port, lfd] =
      tcp_rpc_server(cp, 0, [&](auto conf) { conf->server(server.get()); });

  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = htons(port);
  sockaddr_storage addr{};
  memcpy(&addr, &sin, sizeof(sin));

  // Many connections, many coroutines per connection — maximize
  // concurrent Detached dispatch creation
  unsigned nconns = threads * 2;
  unsigned coros_per_conn = 4;
  DestroyInhibit di;

  // Use a recognizable payload pattern for corruption detection
  Data payload(128, uint8_t{0xA5});

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
    }(cp, addr, di, coros_per_conn, client_count, stop, payload);
  }

  auto start = steady_clock::now();
  auto deadline = start + std::chrono::seconds(seconds);
  while (steady_clock::now() < deadline) {
    poll(nullptr, 0, 5000);
    auto elapsed = duration<double>(steady_clock::now() - start).count();
    std::println("  [{:.1f}s] client RPCs: {}, server dispatches: {}, "
                 "corruptions: {}",
                 elapsed, client_count.load(std::memory_order_relaxed),
                 server_count.load(std::memory_order_relaxed),
                 corruptions.load(std::memory_order_relaxed));
  }

  stop.store(true, std::memory_order_relaxed);
  cp.poison(lfd);
  di.destroy_wait();

  auto elapsed = duration<double>(steady_clock::now() - start);
  auto corrupt = corruptions.load();
  if (corrupt) {
    std::println("\n=== FAIL: {} corruptions detected ===", corrupt);
    return 1;
  }
  std::println("\n=== PASS: No corruption detected ===");
  std::println("Client RPCs: {}, Server dispatches: {}, Duration: {:.2f}s",
               client_count.load(), server_count.load(), elapsed.count());
  return 0;
}
