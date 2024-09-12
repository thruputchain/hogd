// -*-C++-*-

#pragma once

#include <expected>
#include <format>
#include <system_error>

#include <errno.h>

namespace x23 {

// Base type for wrappers around things like errno.  The derived type
// should have an integer-like field err_, and a (static or const)
// method category() that returns a std::error_category&.  Errors
// derived from ErrBase can be pretty-printed with std::format, will
// automatically convert themselves to unexpected when returned in
// functions returning std::expected, and can be thrown as a
// std::exception using raise.
struct ErrBase {
  template<typename Self, typename T, typename E>
  requires std::convertible_to<Self, E>
  operator std::expected<T, E>(this const Self & self) noexcept
  {
    return std::unexpected(self);
  }

  std::error_code code(this const auto &self)
  {
    return std::error_code(int(self.err_), self.category());
  }

  [[noreturn]] void raise(this const auto &self)
  {
    throw std::system_error(self.code());
  }

  template<typename... Args>
  [[noreturn]] void raise(this const auto &self,
                          std::format_string<Args...> fmt, Args &&...args)
  {
    throw std::system_error{self.code(),
                            std::vformat(fmt.get(),
                                         std::make_format_args(args...))};
  }

protected:
  ErrBase() noexcept {}
};

inline std::error_code
make_error_code(const std::derived_from<ErrBase> auto &e)
{
  return e.code();
}

inline std::error_condition
make_error_condition(const std::derived_from<ErrBase> auto &e)
{
  return make_error_code(e).default_error_condition();
}

// Wrapper around errno.  For example:
//
//    if (fstat(fd, &sb) == -1) {
//      Errno e;
//      std::println(stderr, "cannot stat file: {}", e);
//      return e;   // converts to std::unexpected
//    }
struct Errno : ErrBase {
  int err_;
  Errno() noexcept : err_(errno) {}
  Errno(int err) noexcept : err_(err) {}
  static decltype(auto) category() { return std::system_category(); }
  // True if it contains an error code (as apposed to 0)
  explicit operator bool() const { return err_; }
};

} // namespace x23

template<std::derived_from<x23::ErrBase> E>
struct std::is_error_code_enum<E> : std::true_type {};

template<std::derived_from<x23::ErrBase> E>
class std::formatter<E> : public std::formatter<std::string> {
public:
  using super = std::formatter<std::string>;
  auto format(const E &e, auto &ctx) const
  {
    return super::format(make_error_code(e).message(), ctx);
  }
};

namespace x23 {

template<typename A, typename B>
requires requires(const std::error_code ec, const B b) {
  requires std::derived_from<A, x23::ErrBase>;
  requires !std::derived_from<B, x23::ErrBase>;
  { operator==(ec, b) } -> std::same_as<bool>;
}
inline bool
operator==(const A &a, const B &b)
{
  return a.code() == b;
}

} // namespace x23
