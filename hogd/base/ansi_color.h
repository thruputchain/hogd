// ansi_color.h support for ANSI color codes

#pragma once

#include <format>
#include <optional>
#include <string>

#include "hogd/base/string.h"

namespace hogd {

namespace detail {

inline constexpr int kColorOffset = 30;
inline constexpr int kBackgroundOffset = 40;
inline constexpr int kBrightOffset = 90;

} // namespace detail

enum class AnsiColor {
  kBlack = 0,
  kRed = 1,
  kGreen = 2,
  kYellow = 3,
  kBlue = 4,
  kPurple = 5,
  kCyan = 6,
  kWhite = 7,
};

enum class AnsiStyle {
  kNormal = 0,
  kBold = 1,
  kItalic = 3,
  kUnderline = 4,
};

// ansi_color returns an ansi code that sets a style.
constexpr std::string
ansi_color(AnsiStyle style)
{
  auto style_n = static_cast<int>(style);
  return std::format("\e[{}m", style_n);
}

// ansi_color returns an ansi code that sets a style and foreground color.
constexpr std::string
ansi_color(AnsiStyle style, AnsiColor fg)
{
  auto style_n = static_cast<int>(style);
  auto fg_n = static_cast<int>(fg) + detail::kColorOffset;
  return std::format("\e[{};{}m", style_n, fg_n);
}

// ansi_color returns an ansi code that sets a style, foreground color, and
// background color.
constexpr std::string
ansi_color(AnsiStyle style, AnsiColor fg, AnsiColor bg)
{
  auto style_n = static_cast<int>(style);
  auto fg_n = static_cast<int>(fg) + detail::kColorOffset;
  auto bg_n = static_cast<int>(bg) + detail::kBackgroundOffset;
  return std::format("\e[{};{};{}m", style_n, fg_n, bg_n);
}

// ansi_color returns an ansi code that sets a style and bright foreground
// color.
constexpr std::string
ansi_bright_color(AnsiStyle style, AnsiColor fg)
{
  auto style_n = static_cast<int>(style);
  auto fg_n = static_cast<int>(fg) + detail::kBrightOffset;
  return std::format("\e[{};{}m", style_n, fg_n);
}

// ansi_reset returns an ansi code that sets the style and color to normal.
constexpr std::string
ansi_reset()
{
  return "\e[0m";
}

} // namespace hogd
