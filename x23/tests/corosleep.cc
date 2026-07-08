
#include <chrono>
#include <print>

#include <x23/corosleep.h>

x23::ThreadPool pool;
x23::CoroSleep cs(pool);

using myclock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static x23::Async<void>
trysleep()
{
  for (int i = 0; i < 20; ++i) {
    auto start = myclock::now();
    bool not_done = co_await cs.sleep(50ms);
    x23assert(not_done);
    auto stop = myclock::now();
    auto d = stop - start;
    std::println("Waited {:f}s", std::chrono::duration<double>(d).count());
    x23assert(d >= 0.05s && d < 0.15s);
  }
}

int
main()
{
  std::vector<x23::Async<void>> v;

  for (std::size_t i = 0UZ; i < pool.nthreads_; ++i)
    v.emplace_back(trysleep());
  for (auto &a : v)
    a();
  return 0;
}
