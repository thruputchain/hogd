#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <x23/event.h>
#include <x23/sockio.h>
#include <x23/threadpool.h>

using namespace x23;
using namespace std::chrono_literals;

#if USE_EPOLL

namespace {

class TestCoroPoll : public CoroPoll {
public:
  using CoroPoll::CoroPoll;

  void touch_rw(BorrowedFd fd) { (void)get_rw(fd); }
};

template<typename Pred>
bool
wait_until(Pred pred, std::chrono::milliseconds timeout)
{
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::yield();
  }
  return true;
}

} // namespace

int
main()
{
  constexpr unsigned kWorkers = 8;
  constexpr unsigned kIterations = 10'000;
  constexpr uint64_t kWarmupCalls = 2'000;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
    perror("socketpair");
    return 2;
  }

  UniqueFd fd{sv[0]};
  UniqueFd peer{sv[1]};
  set_nonblock(fd);
  set_nonblock(peer);

  ThreadPool pool(2);
  TestCoroPoll cp(pool);

  std::atomic<bool> run{false};
  std::atomic<bool> quit{false};
  std::atomic<unsigned> armed{0};
  std::atomic<unsigned> stopped{0};
  std::atomic<uint64_t> calls{0};

  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (unsigned i = 0; i < kWorkers; ++i) {
    workers.emplace_back([&] {
      for (;;) {
        while (!run.load(std::memory_order_acquire)) {
          if (quit.load(std::memory_order_acquire))
            return;
          run.wait(false, std::memory_order_acquire);
        }
        if (quit.load(std::memory_order_acquire))
          return;

        armed.fetch_add(1, std::memory_order_release);
        while (run.load(std::memory_order_acquire)) {
          cp.touch_rw(fd);
          calls.fetch_add(1, std::memory_order_relaxed);
        }
        stopped.fetch_add(1, std::memory_order_release);
      }
    });
  }

  for (unsigned iter = 0; iter < kIterations; ++iter) {
    uint64_t gen = cp.generation(fd);
    cp.touch_rw(fd);

    armed.store(0, std::memory_order_relaxed);
    stopped.store(0, std::memory_order_relaxed);
    calls.store(0, std::memory_order_relaxed);

    run.store(true, std::memory_order_release);
    run.notify_all();

    if (!wait_until(
            [&] {
              return armed.load(std::memory_order_acquire) == kWorkers &&
                     calls.load(std::memory_order_relaxed) >= kWarmupCalls;
            },
            2s)) {
      std::fprintf(stderr,
                   "FAIL: workers did not start at iteration %u "
                   "(armed=%u/%u calls=%llu)\n",
                   iter, armed.load(std::memory_order_relaxed), kWorkers,
                   static_cast<unsigned long long>(
                       calls.load(std::memory_order_relaxed)));
      _exit(2);
    }

    // generation-aware poison acquires FdState::kLocked, does the epoll
    // deletion, then releases the mode.  Any concurrent get_rw() that saw
    // kLocked is sleeping in FdState::mode().
    cp.poison(fd, gen);

    run.store(false, std::memory_order_release);
    if (!wait_until(
            [&] {
              return stopped.load(std::memory_order_acquire) == kWorkers;
            },
            1s)) {
      std::fprintf(stderr,
                   "FAIL: poison(fd, generation) left a mode() waiter "
                   "asleep at iteration %u (stopped=%u/%u calls=%llu)\n",
                   iter, stopped.load(std::memory_order_relaxed), kWorkers,
                   static_cast<unsigned long long>(
                       calls.load(std::memory_order_relaxed)));
      _exit(1);
    }

    cp.reset(fd);
  }

  quit.store(true, std::memory_order_release);
  run.store(true, std::memory_order_release);
  run.notify_all();
  for (auto &worker : workers)
    worker.join();

  return 0;
}

#else // !USE_EPOLL

int
main()
{
  return 0;
}

#endif // !USE_EPOLL
