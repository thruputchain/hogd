
#include <chrono>
#include <print>

#include <x23/corosleep.h>

x23::ThreadPool pool;
x23::CoroSleep cs(pool);

using myclock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static x23::Async<myclock::duration>
trysleep()
{
  auto start = myclock::now();
  bool not_done = co_await cs.sleep(1s);
  x23assert(not_done);
  auto stop = myclock::now();
  co_return stop - start;
}

int
main()
{
  auto d = trysleep()();
  std::println("Waited {:f}s", std::chrono::duration<double>(d).count());
  x23assert(d >= 1.0s && d < 1.5s);
  return 0;
}
