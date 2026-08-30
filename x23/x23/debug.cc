
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <source_location>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <thread>
#include <tuple>
#include <utility>

#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include "x23/cleanup.h"
#include "x23/config.h"
#include "x23/debug.h"
#include "x23/fileio.h"
#include "x23/progname.h"
#include "x23/signal.h"
#include "x23/vec.h"

#if HAVE_STD_STACKTRACE
#include <stacktrace>
#elif HAVE_BACKTRACE_SYMBOLS
#include <execinfo.h>
#endif // HAVE_BACKTRACE_SYMBOLS

namespace x23 {

bool ignore_debug_env;

void
append_arg(std::string *out, std::string_view arg)
{
  if (!out->empty())
    *out += ' ';

  if (!arg.empty() &&
      arg.find_first_of(" \t\r\n\v\f\"") == std::string_view::npos) {
    *out += arg;
    return;
  }

  *out += '"';
  for (auto c : arg) {
    if (c == '"' || c == '\\')
      *out += '\\';
    *out += c;
  }
  *out += '"';
}

static inline std::pair<std::string_view, std::string_view>
split_arg(std::string_view cmdline)
{
  if (auto pos = cmdline.find('\0'); pos != std::string_view::npos)
    return {cmdline.substr(0, pos), cmdline.substr(pos + 1)};
  return {cmdline, {}};
}

std::string
cmdline()
{
  std::string ret;
  auto r = readfile<std::string>("/proc/self/cmdline");
  if (!r)
    return ret;
  std::string_view rem(*r);
  while (!rem.empty()) {
    std::string_view arg;
    std::tie(arg, rem) = split_arg(rem);
    append_arg(&ret, arg);
  }
  return ret;
}

std::string
stacktrace(unsigned indent, std::string ret)
{
#if HAVE_STD_STACKTRACE
  for (const auto &e : std::stacktrace::current())
    uformat_to(&ret, "{:>{}}{}\n", "", indent, e);
#elif HAVE_BACKTRACE_SYMBOLS
  void *bt[64];
  int n = ::backtrace(bt, 64);
  if (n > 0) {
    char **syms = ::backtrace_symbols(bt, n);
    for (int i = 0; i < n; ++i) {
      ret += "  ";
      ret += syms[i];
      ret += '\n';
    }
    free(syms);
  }
#endif // HAVE_BACKTRACE_SYMBOLS
  return ret;
}

std::string
vformat_line(std::string_view prefix, std::string_view fmt,
             std::format_args args)
{
  std::string msg(prefix);
  uvformat_to(&msg, fmt, args);
  msg += '\n';
  return msg;
}

std::string
vformat_logline(std::string_view prefix, std::string_view fmt,
                std::format_args args)
{
  std::string msg(getprogname());
  if (!msg.empty())
    msg += ": ";
  msg += prefix;
  uvformat_to(&msg, fmt, args);
  msg += '\n';
  return msg;
}

std::string
replace_percent(std::string_view target,
                const std::map<char, std::string> &subst)
{
  std::string out;
  for (size_t i = 0, e = target.size(); i < e; ++i) {
    char c = target[i];
    if (c != '%' || i + 1 == e) {
      out.push_back(c);
      continue;
    }
    c = target[++i];
    if (auto s = subst.find(c); s != subst.end()) {
      out += s->second;
    }
    else {
      if (c != '%')
        out.push_back('%');
      out.push_back(c);
    }
  }
  return out;
}

std::string
expand_log_path(std::string_view path)
{
  static const std::map<char, std::string> expansions = {
      {'f', getprogname()}, {'p', std::to_string(getpid())}};
  return replace_percent(path, expansions);
}

std::string
get_debug_env(const char *var)
{
  if (ignore_debug_env)
    return {};
  if (char *v = getenv(std::format("{}_{}", var, getprogname()).c_str()))
    return v;
  if (char *v = getenv(var))
    return v;
  return {};
}

int
make_debug_fd(const std::string &env_val)
{
  const char *p = env_val.c_str();
  if (const char *comma = std::strrchr(p, ','))
    p = comma + 1;

  if (!*p)
    return -1;

  if (*p == '/' || *p == '.') {
    std::string path = expand_log_path(p);
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd == -1)
      perror(path.c_str());
    return fd;
  }

  char *e;
  if (auto fd = strtoul(p, &e, 10);
      !*e && std::cmp_less_equal(fd, std::numeric_limits<int>::max()))
    return fd;

  return -1;
}

namespace {
inline std::chrono::microseconds
to_duration(const timeval &ts)
{
  using namespace std::chrono;
  return seconds(ts.tv_sec) + microseconds(ts.tv_usec);
}

template<typename R, typename P>
inline double
to_float(const std::chrono::duration<R, P> &d)
{
  return std::chrono::duration<double>(d).count();
}
} // namespace

Defer<>
dump_rusage(int fd)
{
  if (fd < 0)
    return {};
  using std::chrono::steady_clock;
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage)) {
    perror("getrusage");
    return {};
  }

  return [fd, start_time = steady_clock::now(),
          start_utime = to_duration(usage.ru_utime),
          start_stime = to_duration(usage.ru_stime)] {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) {
      perror("getrusage");
      return;
    }
    auto time = steady_clock::now() - start_time;
    auto utime = to_duration(usage.ru_utime) - start_utime;
    auto stime = to_duration(usage.ru_stime) - start_stime;
    auto msg = std::format("{} execution time:\n"
                           "  real {:>9.3f}s\n"
                           "  user {:>9.3f}s\n"
                           "  sys  {:>9.3f}s\n",
                           getprogname(), to_float(time), to_float(utime),
                           to_float(stime));
    ::write(fd, msg.data(), msg.size());
  };
}

#ifdef NDEBUG
namespace detail {
void
do_panic_release(std::string_view fmt, std::format_args args) noexcept
{
  // Best-effort bounded fatal message, then a defined termination.  Formatting
  // allocates and can throw (bad_alloc) on this cold path; never let that
  // escape the noexcept fail-stop -- fall back to a fixed breadcrumb and still
  // abort.
  try {
    std::string msg = getprogname();
    msg += msg.empty() ? "PANIC: " : ":PANIC: ";
    uvformat_to(&msg, fmt, args);
    msg += '\n';
    ::write(2, msg.data(), msg.size());
  } catch (...) {
    std::fputs("x23 PANIC (diagnostic message unavailable)\n", stderr);
  }
  std::abort();
}
} // namespace detail
#else  // !NDEBUG
std::atomic_bool panicking_flag{false};

namespace detail {
void
do_panic(std::string_view fmt, std::format_args args,
         std::source_location sloc) noexcept
{
  std::string msg = getprogname();
  if (!msg.empty())
    msg += ":PANIC: ";
  else
    msg += "PANIC: ";
  uvformat_to(&msg, fmt, args);
  uformat_to(&msg, "\n  in {}\n  function {}\n  at  {}:{}\n", getprogpath(),
             sloc.function_name(), sloc.file_name(), sloc.line());
  if (auto cmd = cmdline(); !cmd.empty())
    uformat_to(&msg, "  Command line:\n    {}\n", cmd);
  if (auto st = stacktrace(4); !st.empty()) {
    msg += "  Stack trace:\n";
    msg += st;
  }
  write(2, msg.data(), msg.size());
  if (int fd = debug.fd(); fd != 2 && fd != -1)
    write(fd, msg.data(), msg.size());
  panicking_flag = true;
  signal_handler(SIGUSR1);
  // Give signal handlers a chance to report debug info.
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::abort();
}
} // namespace detail
#endif // NDEBUG

} // namespace x23
