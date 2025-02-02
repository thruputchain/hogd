#pragma once

#include "hogd/hogexec/runtime/syscall_entry.h"
#include "x23/format.h"
#include "x23/x23.h"
#include <print>

template<> struct std::formatter<std::span<const std::byte>> {
  constexpr static auto parse(auto &ctx) noexcept { return ctx.begin(); }

  static auto format(std::span<const std::byte> const &obj, auto &ctx)
  {
    // NOLINTNEXTLINE(readability-qualified-auto)
    auto out = ctx.out();
    for (auto v : obj) {
      uint8_t byte = static_cast<uint8_t>(v);
      out = std::format_to(out, "{:02x}", byte);
    }
    return out;
  }
};

namespace hogd {

template<typename T>
std::string
log_syscall_argument_noopt(T const &arg)
{
  if constexpr (x23::is_xdr_type<T>) {
    return x23::show(arg);
  }
  else {
    return std::format("{}", arg);
  }
}

template<typename T>
std::string
log_syscall_argument(T const &arg)
{
  if constexpr (std::is_pointer_v<T>) {
    if (arg) {
      return log_syscall_argument_noopt(*arg);
    }
    return "NULLPTR";
  }
  else {
    return log_syscall_argument_noopt(arg);
  }
}

inline std::string
log_syscall_arguments()
{
  return "";
}

template<typename T, typename... Args>
std::string
log_syscall_arguments(T const &arg, Args const &...args)
{
  // this is an annoying and inefficient way to serialize, but i don't care
  return std::format("{}  ", log_syscall_argument(arg)) +
         log_syscall_arguments(args...);
}

inline static void
log_syscall_parse_failure(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5,
                          uint64_t arg6)
{
  if (syscall_logfd == -1) {
    return;
  }
  auto msg =
      std::format("Syscall argument parse error: [{}] {} {} {} {} {} {}\n",
                  arg0, arg1, arg2, arg3, arg4, arg5, arg6);
  write(syscall_logfd, msg.data(), msg.size());
}

template<typename... T>
void
log_syscall(SyscallNo syscall, T const &...args)
{
  if (syscall_logfd == -1) {
    return;
  }

  auto msg = std::format("{}: {}\n", x23::show(syscall),
                         log_syscall_arguments(args...));
  write(syscall_logfd, msg.data(), msg.size());
}

} // namespace hogd
