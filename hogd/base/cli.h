// cli.h - helpers for parsing command-line arguments in daemon startup paths.

#pragma once

#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <string_view>
#include <system_error>

#include "x23/sockio.h"

namespace hogd {

// die reports a fatal startup error and exits.
//
// Bad command-line input is a user error, not a violated invariant: report it
// and exit rather than terminating on an uncaught throw.
[[noreturn]] inline void
die(std::string_view message)
{
  std::println(stderr, "{}", message);
  exit(EXIT_FAILURE);
}

// parse_integer_or_die parses a complete, in-range number in the given base,
// or exits via die() naming the offending flag. A base of 0 auto-detects: a
// "0x" prefix means hex, anything else is decimal.
//
// It rejects trailing characters, so hogd::parse_integer (which accepts them:
// "80abc" -> 80) is not a substitute. std::stoul / std::stoi are not either:
// they throw on non-numeric or out-of-range input.
//
// Unlike std::stoul with base 0, a leading zero does NOT mean octal: "0755" is
// 755. No flag here takes an octal argument, and silent octal is a footgun.
template<std::integral T>
T
parse_integer_or_die(std::string_view label, std::string_view value,
                     int base = 10)
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
    die(std::format("Invalid {}: {}", label, value));
  }
  return parsed;
}

// parse_port parses a TCP/UDP port, or exits. Out-of-range values are rejected
// by the uint16_t parse itself.
inline uint16_t
parse_port(std::string_view value)
{
  return parse_integer_or_die<uint16_t>("port", value);
}

// parse_host_port_or_die parses a joined "host:port" address, or exits naming
// the offending flag.
inline x23::HostPort
parse_host_port_or_die(std::string_view label, std::string_view address)
{
  x23::HostPort host_port;
  if (!host_port.parse(address)) {
    die(std::format("Invalid {}: {}", label, address));
  }
  return host_port;
}

} // namespace hogd
