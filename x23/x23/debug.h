// -*-C++-*-

#pragma once

#include <format>
#include <map>
#include <string>

#include <unistd.h>

#include <x23/utility.h>

namespace x23 {

// Do not do any logging regardless of environment variables
extern bool ignore_debug_env;

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
template<std::size_t N> requires (N > 0)
struct FixedStr {
  char value[N];
  consteval FixedStr(const char (&v)[N]) noexcept { std::copy_n(v, N, value); }

  constexpr operator const char *() const noexcept { return value; }
};

template<FixedStr EnvVar> struct DebugFd {
  static inline std::string env_val;

  static int fd() noexcept
  {
    static int the_fd = [] {
      env_val = get_debug_env(EnvVar);
      return make_debug_fd(env_val);
    }();
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

private:
  static void do_vfmt(std::string_view fmt, std::format_args args)
  {
    std::string msg = std::vformat(fmt, args);
    msg += '\n';
    write(fd(), msg.data(), msg.size());
  }
};

struct NullDebugFd {
  static inline const std::string env_val{};

  static constexpr int fd() noexcept { return -1; }
  constexpr operator bool() const { return false; }
  static void vfmt(std::string_view fmt, std::format_args args) {}
  static void fmt(auto &&...) {}
  static void operator()(auto &&...) {}
};

#ifdef NDEBUG
inline NullDebugFd debug;
#else  // !NDEBUG
inline DebugFd<"X23SERVER_TRACE"> debug;
#endif // !NDEBUG

} // namespace x23
