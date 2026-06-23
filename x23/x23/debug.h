// -*-C++-*-

#pragma once

#include <atomic>
#include <cstdio>
#include <exception>
#include <format>
#include <map>
#include <source_location>
#include <string>
#include <string_view>
#include <unistd.h>

#include <x23/cleanup.h>
#include <x23/utility.h>

namespace x23 {

// Do not do any logging regardless of environment variables
extern bool ignore_debug_env;

// Prepend prefix and append a newline to a formatted string.
std::string vformat_line(std::string_view prefix, std::string_view fmt,
                         std::format_args args);

// Like vformat_log_msg but add the program name at the beginning.
std::string vformat_logline(std::string_view prefix, std::string_view fmt,
                            std::format_args args);

// Replace instances of "%n" for various characters 'n' with a
// substitution string.  "%%" is replaced with "%".
std::string replace_percent(std::string_view target,
                            const std::map<char, std::string> &substitutions);

// Replace '%f' with program name and '%p' with pid.
std::string expand_log_path(std::string_view path);

// If var contains "VAR", look up first "VAR_PROGNAME" (where progname
// is the name of the current program) then "VAR".  Return an empty
// string if the environment variable is not set of if
// ignore_debug_env is true.
std::string get_debug_env(const char *var);

// Make a file descriptor for debugging based on a string which can be
// either a numeric file descriptor or a pathname starting with '.' or
// '/'.  Returns -1 if env_val is in neither format.  If there's a
// comma in the string, skips over everything up to and including the
// comma.
int make_debug_fd(const std::string &env_val);

// A comple-time string that's suitable as a non-type template
// argument.
template<size_t N> requires (N > 0)
struct FixedStr {
  char value[N];
  consteval FixedStr(const char (&v)[N]) noexcept { std::copy_n(v, N, value); }

  constexpr operator const char *() const noexcept { return value; }
};

template<FixedStr EnvVar> struct DebugFd {
  // NOLINTNEXTLINE(readability-identifier-naming)
  static inline std::string env_val;

  __attribute__((no_sanitize("thread"))) static int fd() noexcept
  {
    static int the_fd = make_fd();
    return the_fd;
  }

  operator bool() const { return fd() >= 0; }

  static void vfmt(std::string_view fmt, std::format_args args)
  {
    if (fd() >= 0)
      do_vfmt(fmt, args);
  }

  template<typename... Args>
  static void fmt(std::format_string<Args...> fmt, Args &&...args)
  {
    if (fd() >= 0)
      do_vfmt(fmt.get(), std::make_format_args(args...));
  }

  template<typename... Args>
  static void operator()(std::format_string<Args...> fmt, Args &&...args)
  {
    if (fd() >= 0)
      do_vfmt(fmt.get(), std::make_format_args(args...));
  }

  static void verr(std::string_view fmt, std::format_args args)
  {
    std::string msg = vformat_logline("", fmt, args);
    if (int f = fd(); f != -1 && f != 2)
      write(fd(), msg.data(), msg.size());
    write(2, msg.data(), msg.size());
  }
  template<typename... Args>
  static void err(std::format_string<Args...> fmt, Args &&...args)
  {
    verr(fmt.get(), std::make_format_args(args...));
  }

private:
  static void do_vfmt(std::string_view fmt, std::format_args args)
  {
    std::string msg = vformat_logline("", fmt, args);
    write(fd(), msg.data(), msg.size());
  }

  static int make_fd() noexcept
  {
    try {
      env_val = get_debug_env(EnvVar);
    } catch (std::exception const &e) {
      // make_fd() is noexcept and lazily runs at first use from callers that
      // are themselves noexcept (e.g. the test FFI create_db() start trace,
      // #2641). get_debug_env() allocates a std::string, so a std::bad_alloc
      // here would cross the noexcept boundary and terminate the process.
      // Disable this debug fd instead: leave env_val empty and report
      // best-effort to stderr without allocating.
      env_val.clear();
      std::fputs("x23 debug: failed to read debug env: ", stderr);
      std::fputs(e.what(), stderr);
      std::fputc('\n', stderr);
      return -1;
    } catch (...) {
      // make_fd() is a shared x23 helper consumed beyond the test FFI, so guard
      // a non-std::exception throw too: otherwise it would still hit the
      // noexcept specifier and terminate (parallel to the catch(...) on the
      // test FFI root_invoke()). No exception object to format here.
      env_val.clear();
      std::fputs("x23 debug: failed to read debug env (unknown exception)\n",
                 stderr);
      return -1;
    }
    return make_debug_fd(env_val);
  }
};

struct NullDebugFd {
  // NOLINTNEXTLINE(readability-identifier-naming)
  static inline const std::string env_val{};

  static constexpr int fd() noexcept { return -1; }
  constexpr operator bool() const { return false; }
  static void vfmt(std::string_view fmt, std::format_args args) {}
  static void fmt(auto &&.../*unused*/) {}
  static void operator()(auto &&.../*unused*/) {}
  static void verr(std::string_view fmt, std::format_args args)
  {
    std::string msg = vformat_logline("", fmt, args);
    write(2, msg.data(), msg.size());
  }
  template<typename... Args>
  static void err(std::format_string<Args...> fmt, Args &&...args)
  {
    verr(fmt.get(), std::make_format_args(args...));
  }
};

#ifdef NDEBUG
inline NullDebugFd debug;
#else  // !NDEBUG
inline DebugFd<"X23SERVER_TRACE"> debug;
#endif // !NDEBUG

// Dump the CPU time used between when this function is called and the
// result is destroyed.  Usage:
//
//    int
//    main(int argc, char **argv)
//    {
//       auto _rusage = dump_rusage(1);
//       // ...
//    }
// NOLINTNEXTLINE(readability-static-accessed-through-instance)
Defer<> dump_rusage(int fd = debug.fd());

// Append argument to string, quoting if necessary
void append_arg(std::string *out, std::string_view arg);

// If /proc/self/cmdline exists, parse it into a single string
std::string cmdline();

std::string stacktrace(unsigned indent = 2, std::string ret = {});

#ifdef NDEBUG

template<typename... Args>
[[noreturn]] inline void
panic(std::format_string<Args...>, const Args &...args)
{
  std::unreachable();
}

consteval bool
panicking()
{
  return false;
}

#define x23assert(cond)

#else // !NDEBUG

namespace detail {
[[noreturn]] void do_panic(std::string_view fmt, std::format_args args,
                           std::source_location sloc) noexcept;
} // namespace detail

// NOLINTNEXTLINE(readability-identifier-naming)
template<typename... Args> struct panic {
  panic(std::format_string<Args...> fmt, const Args &...args,
        std::source_location sloc = std::source_location::current()) noexcept
  {
    detail::do_panic(fmt.get(), std::make_format_args(args...), sloc);
  }
};
template<typename... Args>
panic(std::format_string<Args...>, const Args &...args) -> panic<Args...>;

#define x23assert(cond)                                                        \
  do {                                                                         \
    if (!(cond)) [[unlikely]]                                                  \
      ::x23::panic("x23assert failure: {}", #cond);                            \
  } while (0)

extern std::atomic_bool panicking_flag;
inline bool
panicking() noexcept
{
  return panicking_flag.load(std::memory_order_relaxed);
}

#endif // !NDEBUG

} // namespace x23
