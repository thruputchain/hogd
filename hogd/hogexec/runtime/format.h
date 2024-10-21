#pragma once

// Utilities for printing runtime types

#include <array>
#include <cstdint>
#include <format>
#include <vector>

#include "syscall.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/vec.h"

template<size_t N> struct std::formatter<std::array<unsigned char, N>> {
  constexpr static auto parse(auto &ctx) noexcept { return ctx.begin(); }

  static auto format(const std::array<uint8_t, N> &obj, auto &ctx)
  {
    // clang-tidy is incorrect about this: it wants auto*, not auto
    // NOLINTNEXTLINE(readability-qualified-auto)
    auto out = ctx.out();
    for (auto v : obj) {
      out = std::format_to(out, "{:02x}", v);
    }
    return out;
  }
};

template<> struct std::formatter<::hogd::AddressAndKey> {
  constexpr static auto parse(auto &ctx) noexcept { return ctx.begin(); }

  static auto format(const ::hogd::AddressAndKey &v, auto &ctx)
  {
    return std::format_to(ctx.out(), "Address {} Key {}", v.address, v.key);
  }
};

template<size_t N, typename Allocator>
struct std::formatter<x23::Vec<uint8_t, N, Allocator>> {
  constexpr static auto parse(auto &ctx) noexcept { return ctx.begin(); }

  static auto format(const x23::Vec<uint8_t, N, Allocator> &obj, auto &ctx)
  {
    // NOLINTNEXTLINE(readability-qualified-auto)
    auto out = ctx.out();
    for (auto v : obj) {
      out = std::format_to(out, "{:02x}", v);
    }
    return out;
  }
};

template<> struct std::formatter<hogd::SyscallNo> {
  constexpr static auto parse(auto &ctx) noexcept { return ctx.begin(); }

  static auto format(const hogd::SyscallNo &obj, auto &ctx)
  {
    return std::format_to(ctx.out(), "SyscallNo::{}", x23::show(obj));
  }
};

template<> struct std::formatter<hogd::SyscallErrorCode> {
  constexpr static auto parse(auto &ctx) noexcept { return ctx.begin(); }

  static auto format(const hogd::SyscallErrorCode &obj, auto &ctx)
  {
    return std::format_to(ctx.out(), "SyscallErrorCode::{}", x23::show(obj));
  }
};
