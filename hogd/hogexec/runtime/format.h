#pragma once

// Utilities for printing runtime types

#include <format>

#include "syscall.hh"
#include "x23/vec.h"

template<size_t N> struct std::formatter<std::array<uint8_t, N>> {
  constexpr static auto parse(format_parse_context &ctx) { return ctx.begin(); }

  auto format(const std::array<uint8_t, N> &obj, std::format_context &ctx)
  {
    auto out = ctx.out();
    for (auto v : obj) {
      out = std::format_to(out, "{:02x}", v);
    }
    return out;
  }
};

template<size_t N, typename Allocator>
struct std::formatter<x23::Vec<uint8_t, N, Allocator>> {
  constexpr static auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const x23::Vec<uint8_t, N, Allocator> &obj,
                     std::format_context &ctx)
  {
    auto out = ctx.out();
    for (auto v : obj) {
      out = std::format_to(out, "{:02x}", v);
    }
    return out;
  }
};

template<> struct std::formatter<hogd::SyscallNo> {
  constexpr static auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::SyscallNo &obj, std::format_context &ctx)
  {
    auto const &tags = x23::xdr_meta<hogd::SyscallNo>::tags;

    for (auto const &[enum_val, string] : tags) {
      if (enum_val == obj) {
        return std::format_to(ctx.out(), "SyscallNo: {}", string);
      }
    }
    return std::format_to(ctx.out(), "SyscallNo: {} (Unknown)",
                          static_cast<int32_t>(obj));
  }
};

template<> struct std::formatter<hogd::SyscallErrorCode> {
  constexpr static auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::SyscallErrorCode &obj,
                     std::format_context &ctx)
  {
    auto const &tags = x23::xdr_meta<hogd::SyscallErrorCode>::tags;

    for (auto const &[enum_val, string] : tags) {
      if (enum_val == obj) {
        return std::format_to(ctx.out(), "SyscallErrorCode: {}", string);
      }
    }
    return std::format_to(ctx.out(), "SyscallErrorCode: {} (Unknown)",
                          static_cast<int32_t>(obj));
  }
};
