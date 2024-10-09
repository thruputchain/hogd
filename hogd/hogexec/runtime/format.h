#pragma once

// Utilities for printing runtime types

#include <format>

#include "x23/vec.h"

template<size_t N> struct std::formatter<std::array<uint8_t, N>> {
  constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  auto format(const std::array<uint8_t, N> &obj, std::format_context &ctx) const
  {
    auto out = ctx.out();
    for (auto v : obj) {
      out = std::format_to(out, "{:02x}", v);
    }
    return out;
  }
};

template<size_t N, typename allocator>
struct std::formatter<x23::Vec<uint8_t, N, allocator>> {
  constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  auto format(const x23::Vec<uint8_t, N, allocator> &obj,
              std::format_context &ctx) const
  {
    auto out = ctx.out();
    for (auto v : obj) {
      out = std::format_to(out, "{:02x}", v);
    }
    return out;
  }
};
