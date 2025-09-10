
#include <atomic>
#include <print>
#include <stop_token>

#include <sys/socket.h>
#include <unistd.h>

#include <x23/event.h>
#include <x23/sockio.h>

using namespace x23;

struct CoroWake : CoroPoll {
  static inline std::atomic_uint64_t idctr{};

  uint64_t id_;
  std::atomic_uint64_t loopcnt_{};

  CoroWake(ThreadPool &pool) : CoroPoll(pool, no_loop_thread{}), id_(idctr++)
  {
    pollthread_ = std::thread(&CoroWake::loop, this);
    std::println("Created CoroWake {}", id_);
  }
  ~CoroWake() { std::println("Destroying CoroWake {}", id_); }

  using CoroPoll::wake;

  void loop()
  {
    while (!time_to_stop()) {
      check();
      loopcnt_.fetch_add(1, std::memory_order_relaxed);
      loopcnt_.notify_all();
    }
  }
};

void
simple_wake(ThreadPool &pool)
{
  CoroWake cp(pool);

  for (int i = 0; i < 100; ++i) {
    auto n = cp.loopcnt_.load();
    cp.wake();
    cp.loopcnt_.wait(n);
  }
}

void
reset_fd(ThreadPool &pool)
{
  CoroWake cp(pool);

  auto lfd = tcplisten(0, 1000);
  auto dest = local_addr(lfd);

  [&cp, lfd = std::move(lfd)](this auto) -> Detached {
    sockaddr_storage peer;
    while (auto fd = co_await cp.accept(lfd, &peer)) {
      [&cp, fd = UniqueFd{*fd}](this auto) -> Detached {
        Buf b;
        co_await cp.read(fd, &b, 1);
	enable_abortive_close(fd); // avoid TIME_WAIT
      }();
    }
    co_return;
  }();

  DestroyInhibit di;
  for (int i = 0; i < 1000; ++i)
    [&cp, dest, r = RefPtr{di}](this auto) -> Detached {
      UniqueFd fd{socket(dest.ss_family, SOCK_STREAM, 0)};
      if (auto res = co_await cp.connect(fd, &dest); !res)
        std::println(stderr, "connect: {}", res.error());
      [[maybe_unused]] auto n = write(fd.get(), "A", 1);
      assert(n == 1);
      Buf b;
      co_await cp.read(fd, &b, 1);
    }();

  di.destroy_wait();
}

int
main()
{
  setlinebuf(stdout);
  ThreadPool pool;
  simple_wake(pool);
  reset_fd(pool);
}
