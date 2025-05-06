// -*-C++-*-

#pragma once

#include <cerrno>
#include <concepts>
#include <expected>
#include <format>
#include <string>
#include <system_error>

#include <type_traits>

namespace x23 {

// ErrorBase provides a wrapper for all error types. It provides
// pretty-printing through std::format, automatic conversion to unexpected when
// returning from functions with std::expected, and compatibility with the std
// library's error handling. It can also be thrown as a std::system_error via
// raise().
//
// ErrorBase uses explicit object parameters to avoid the overhead of a vtable.
//
// The error class must inherit ErrorBase and provide the following methods:
// 1. int raw_code()
// 2. std::error_category category() // usually static
//
// In addition, it should include its own operator== definition.
class ErrorBase {
public:
  // provides automatic conversion to std::unexpected
  template<typename Self, typename T, typename E>
  requires std::convertible_to<Self, E>
  [[nodiscard]] operator std::expected<T, E>(this const Self & self) noexcept
  {
    return std::unexpected(self);
  }

  // error_code converts the derived error into a std::error_code.
  std::error_code error_code(this const auto &self)
  {
    return std::error_code(self.raw_code(), self.category());
  }

  // raise converts the derived error to a system error and throws it.
  [[noreturn]] void raise(this const auto &self)
  {
    throw std::system_error(self.error_code());
  }

  // raise converts the derived error to a system error that includes additional
  // text and throws it.
  template<typename... Args>
  [[noreturn]] void raise(this const auto &self,
                          std::format_string<Args...> fmt, Args &&...args)
  {
    throw std::system_error{self.error_code(),
                            std::vformat(fmt.get(),
                                         std::make_format_args(args...))};
  }

protected:
  ErrorBase() noexcept = default;
};

// make_error_code converts an ErrorBase derived class into a std::error_code.
[[nodiscard]] inline std::error_code
make_error_code(const std::derived_from<ErrorBase> auto &e)
{
  return e.error_code();
}

// make_error_condition converts an ErrorBase derived class into a
// std::error_condition.
[[nodiscard]] inline std::error_condition
make_error_condition(const std::derived_from<ErrorBase> auto &e)
{
  return make_error_code(e).default_error_condition();
}

} // namespace x23

// make everything that inherits ErrorBase an error_code_enum
template<std::derived_from<x23::ErrorBase> E>
struct std::is_error_code_enum<E> : std::true_type {};

// provide format support for everything that inherits ErrorBase
template<std::derived_from<x23::ErrorBase> E>
class std::formatter<E> : public std::formatter<std::string> {
public:
  using super = std::formatter<std::string>;
  auto format(const E &e, auto &ctx) const
  {
    return super::format(make_error_code(e).message(), ctx);
  }
};

namespace x23 {

// Errno provides a wrapper around errno.
//
// Example:
// if (fstat(fd, &sb) == -1) {
//   Errno e;
//   std::println(stderr, "cannot stat file: {}", e);
//   return e;   // converts to std::unexpected
// }
class Errno : public ErrorBase {
public:
  Errno() noexcept : err_(errno) {}
  Errno(int err) noexcept : err_(err) {}

  int raw_code() const { return err_; }
  static const std::error_category &category()
  {
    return std::system_category();
  }
  explicit operator bool() const { return err_ != 0; }

private:
  int err_;
};

// Enable comparisons between Errno and legacy errno codes.
inline bool
operator==(const Errno &lhs, const int &rhs)
{
  return lhs.raw_code() == rhs;
}

// Enable comparisons between Errno objects.
template<std::derived_from<ErrorBase> E>
inline bool
operator==(const E &lhs, const E &rhs)
{
  return lhs.raw_code() == rhs.raw_code();
}

template<typename A, typename B>
requires requires(const std::error_code ec, const B b) {
  requires std::derived_from<A, x23::ErrorBase>;
  requires !std::derived_from<B, x23::ErrorBase>;
  { operator==(ec, b) } -> std::same_as<bool>;
}
inline bool
operator==(const A &a, const B &b)
{
  return a.error_code() == b;
}

} // namespace x23
