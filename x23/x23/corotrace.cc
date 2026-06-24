#include "x23/corotrace.h"

#include <ctime>
#include <exception>
#include <string>
#include <unistd.h>
#include <utility>

#include "x23/debug.h"
#include "x23/vec.h"

namespace x23 {

namespace {
thread_local CoroCpuAccount *current_cpu_account{};
thread_local uint64_t current_cpu_account_start{};
} // namespace

uint64_t
thread_cpu_time_ns() noexcept
{
#ifdef CLOCK_THREAD_CPUTIME_ID
  timespec ts{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000UL +
           static_cast<uint64_t>(ts.tv_nsec);
#endif // CLOCK_THREAD_CPUTIME_ID
  return 0;
}

namespace detail {

CoroCpuAccount *
current_coro_cpu_account() noexcept
{
  return current_cpu_account;
}

void
switch_coro_cpu_account(CoroCpuAccount *account) noexcept
{
  const uint64_t now = thread_cpu_time_ns();
  if (current_cpu_account != nullptr && now >= current_cpu_account_start)
    current_cpu_account->ns.fetch_add(now - current_cpu_account_start,
                                      std::memory_order_relaxed);
  current_cpu_account = account;
  current_cpu_account_start = now;
}

ScopedCoroCpuAccount::ScopedCoroCpuAccount(CoroCpuAccount *account) noexcept
  : previous_(current_cpu_account)
{
  switch_coro_cpu_account(account);
}

ScopedCoroCpuAccount::~ScopedCoroCpuAccount()
{
  switch_coro_cpu_account(previous_);
}

} // namespace detail

std::string
trace(const PromiseBase *pb, unsigned indent, std::string ret)
{
#if X23_CORO_TRACE
  for (PromiseBase *next; pb; pb = next) {
    uformat_to(&ret,
               "{0:>{1}}{2}\n"
               "{0:>{1}}  suspended at {3}:{4}\n",
               "", indent, pb->suspension_.function_name(),
               pb->suspension_.file_name(), pb->suspension_.line());
    next = pb->waiter_;
    if (!next)
      uformat_to(&ret, "{0:>{1}}  created at {2}:{3}\n", "", indent,
                 pb->creation_.file_name(), pb->creation_.line());
  }
#endif // X23_CORO_TRACE
  return ret;
}

void
trace_unhandled_exception(const PromiseBase *pb) noexcept
{
  std::string msg("Unhandled exception in coroutine:\n");
  std::exception_ptr p = std::current_exception();
  try {
    rethrow_exception(p);
  } catch (const std::exception &e) {
    uformat_to(&msg, "What: {}\n", e.what());
  } catch (...) {
    msg += "Exception type not derived from std::exception\n";
  }
  {
    static const std::string stmsg("Stack trace:\n");
    msg += stmsg;
    size_t oldlen = msg.size();
    msg = stacktrace(2, std::move(msg));
    if (msg.size() == oldlen)
      msg.resize(msg.size() - stmsg.size());
  }
#if X23_CORO_TRACE
  uformat_to(&msg, "Coroutine trace:\n");
  msg = trace(pb, 2, std::move(msg));
#endif // X23_CORO_TRACE
  ::write(2, msg.data(), msg.size());
  rethrow_exception(p); // Will terminate since we are noexcept
}

} // namespace x23
