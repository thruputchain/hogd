// Single-file reproducer: clang 22 emits, in a coroutine's .destroy clone, a
// flag-guarded destroy of the deferred-conversion return object (declared
// return type MaybeTask<T>, get_return_object() returns Task<T> via
// coroutine_traits inheritance).  The cleanup's stack slot is never
// initialized in the destroy clone, so destroy() runs on stack garbage.
// Crashes at -O0 with clang 22.1.5; fine with clang 21.
//   clang++ -std=c++20 -O0 standalone.cc && ./a.out
#include <coroutine>
#include <cstdio>
#include <semaphore>
#include <thread>
#include <utility>
#include <variant>

template<typename R> struct Task {
  struct promise_type;
  using handle_t = std::coroutine_handle<promise_type>;
  struct promise_type {
    R value{};
    std::coroutine_handle<> caller{};
    Task get_return_object() { return Task{handle_t::from_promise(*this)}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    struct Tail {
      promise_type *p;
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
        return p->caller ? p->caller : std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };
    Tail final_suspend() noexcept { return {this}; }
    void return_value(R r) { value = std::move(r); }
    void unhandled_exception() { std::terminate(); }
  };
  handle_t h{};
  explicit Task(handle_t hh) : h(hh) {}
  Task(Task &&o) noexcept : h(std::exchange(o.h, {})) {}
  ~Task() { if (h) h.destroy(); }
  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> c) { h.promise().caller = c; }
  R await_resume() { return std::move(h.promise().value); }
};

template<typename T> struct MaybeTask : std::variant<Task<T>, T> {
  using std::variant<Task<T>, T>::variant;
  bool await_ready() { return this->index() == 1; }
  void await_suspend(std::coroutine_handle<> c) {
    std::get<0>(*this).await_suspend(c);
  }
  T await_resume() {
    return this->index() == 1 ? std::move(std::get<1>(*this))
                              : std::get<0>(*this).await_resume();
  }
};
template<typename T, typename... Args>
struct std::coroutine_traits<MaybeTask<T>, Args...>
  : std::coroutine_traits<Task<T>, Args...> {};

// External-completion leaf: suspends, parks its handle for another thread.
static std::coroutine_handle<> g_parked;
static std::binary_semaphore g_ready{0};
struct ParkAwaiter {
  bool await_ready() { return false; }
  void await_suspend(std::coroutine_handle<> h) {
    g_parked = h;
    g_ready.release();
  }
  int await_resume() { return 41; }
};

static Task<int> leaf() { co_return co_await ParkAwaiter{}; }

static MaybeTask<std::pair<int, long>> inner()
{
  int v = co_await leaf();
  co_return std::pair<int, long>{v + 1, v + 2};
}

static std::binary_semaphore g_done{0};
static int g_result;

static Task<int> outer()
{
  MaybeTask<std::pair<int, long>> ma = inner();
  std::pair<int, long> r = co_await ma; // ~MaybeTask destroys inner's frame
  g_result = r.first + (int)r.second;   // on the resuming (dirty) thread
  g_done.release();
  co_return 0;
}

__attribute__((noinline)) static void dirty_stack()
{
  volatile unsigned long long buf[2048];
  for (auto &x : buf)
    x = 0x5151515151515151ULL;
}

int main()
{
  for (int i = 0; i < 50; i++) {
    Task<int> t = outer();
    g_ready.acquire();
    std::thread completer([] {
      dirty_stack();
      g_parked.resume();
    });
    g_done.acquire();
    completer.join();
    if (g_result != 85) { std::printf("iter %d: wrong %d\n", i, g_result); return 1; }
  }
  std::printf("ok\n");
  return 0;
}
