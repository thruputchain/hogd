// cli.h - parsers for command-line argument values.
//
// These only parse: they return std::nullopt on bad input and leave the policy
// to the caller. Daemons report the offending flag and call their usage(),
// which is friendlier than a panic for what is usually a typo.

#pragma once

#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <print>
#include <string_view>
#include <system_error>

namespace hogd {

// try_parse_integer parses a complete, in-range number in the given base. A
// base of 0 auto-detects: a "0x" prefix means hex, anything else is decimal.
//
// It rejects trailing characters, so hogd::parse_integer (which accepts them:
// "80abc" -> 80) is not a substitute. std::stoul / std::stoi are not either:
// they throw on non-numeric or out-of-range input.
//
// Unlike std::stoul with base 0, a leading zero does NOT mean octal: "0755" is
// 755. No flag takes an octal argument, and silent octal is a footgun.
template<std::integral T>
std::optional<T>
try_parse_integer(std::string_view value, int base = 10)
{
  // std::from_chars rejects a "0x" prefix, but std::stoul (which this replaces)
  // accepted one for base 16 and base 0, so keep taking it.
  std::string_view digits = value;
  const bool hex_prefix = digits.starts_with("0x") || digits.starts_with("0X");
  if (base == 0) {
    base = hex_prefix ? 16 : 10;
  }
  if (base == 16 && hex_prefix) {
    digits.remove_prefix(2);
  }

  T parsed{};
  const char *const begin = digits.data();
  const char *const end = begin + digits.size();
  const auto res = std::from_chars(begin, end, parsed, base);
  if (res.ec != std::errc{} || res.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

// try_parse_port parses a TCP/UDP port. Out-of-range values are rejected by the
// uint16_t parse itself.
inline std::optional<uint16_t>
try_parse_port(std::string_view value)
{
  return try_parse_integer<uint16_t>(value);
}

// UsageFn is a daemon's usage(): prints the usage message and exits.
using UsageFn = void (*)(char *argv[]);

// parse_integer_arg parses a flag value, or names the offending flag and hands
// off to the caller's usage(). A mistyped flag is a user error, so it gets the
// usage message rather than a panic and a stack trace.
template<std::integral T>
T
parse_integer_arg(std::string_view label, std::string_view value, char *argv[],
                  UsageFn usage, int base = 10)
{
  std::optional<T> parsed = try_parse_integer<T>(value, base);
  if (parsed) {
    return *parsed;
  }
  std::println(stderr, "Invalid {}: {}", label, value);
  usage(argv);
  exit(EXIT_FAILURE); // only if usage() returns, which it should not
}

inline uint16_t
parse_port_arg(std::string_view label, std::string_view value, char *argv[],
               UsageFn usage)
{
  return parse_integer_arg<uint16_t>(label, value, argv, usage);
}

// arg_error reports a rejected command-line value and hands off to the caller's
// usage(). It exists for validation the parse helpers cannot express (mutually
// exclusive flags, empty paths, out-of-range counts): the caller does its own
// check and calls this with a ready-made message. The "error:" prefix matches
// what these daemons already print when they still translate a failure through
// a top-level catch, so the two paths read the same.
[[noreturn]] inline void
arg_error(std::string_view message, char *argv[], UsageFn usage)
{
  std::println(stderr, "error: {}", message);
  usage(argv);
  exit(EXIT_FAILURE); // only if usage() returns, which it should not
}

} // namespace hogd
