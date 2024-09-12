// ansi_color.h support for ANSI color codes

#pragma once

#include <format>
#include <string>

namespace hogd {

namespace detail {

inline constexpr int kColorOffset = 30;
inline constexpr int kBackgroundOffset = 40;

} // namespace detail

enum class Color {
  kBlack = 0,
  kRed = 1,
  kGreen = 2,
  kYellow = 3,
  kBlue = 4,
  kPurple = 5,
  kCyan = 6,
  kWhite = 7,
  kBrightBlack = 60,
  kBrightRed = 61,
  kBrightGreen = 62,
  kBrightYellow = 63,
  kBrightBlue = 64,
  kBrightPurple = 65,
  kBrightCyan = 66,
  kBrightWhite = 67,
};

enum class Style {
  kNormal = 0,
  kBold = 1,
  kItalic = 3,
  kUnderline = 4,
};

// ansi_color returns an ansi code that sets a style.
constexpr std::string
ansi_color(Style style)
{
  auto style_n = static_cast<int>(style);
  return std::format("\e[{}m", style_n);
}

// ansi_color returns an ansi code that sets a style and foreground color.
constexpr std::string
ansi_color(Style style, Color fg)
{
  auto style_n = static_cast<int>(style);
  auto fg_n = static_cast<int>(fg) + detail::kColorOffset;
  return std::format("\e[{};{}m", style_n, fg_n);
}

// ansi_color returns an ansi code that sets a style, foreground color, and
// background color.
constexpr std::string
ansi_color(Style style, Color fg, Color bg)
{
  auto style_n = static_cast<int>(style);
  auto fg_n = static_cast<int>(fg) + detail::kColorOffset;
  auto bg_n = static_cast<int>(bg) + detail::kBackgroundOffset;
  return std::format("\e[{};{};{}m", style_n, fg_n, bg_n);
}

// ansi_reset returns an ansi code that sets the style and color to normal.
constexpr std::string
ansi_reset()
{
  return "\e[0m";
}

} // namespace hogd
