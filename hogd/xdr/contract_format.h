// -*-C++-*-

#include <format>

#include "hogd/crypto/crypto.h"

template<> struct std::formatter<hogd::ContractBlob> {
  constexpr auto parse(auto &ctx) { return ctx.begin(); }
  auto format(const hogd::ContractBlob &blob, auto &ctx) const
  {
    auto sha = hogd::sha256(blob.wasm);
    std::format_to(ctx.out(), "ContractBlob(SHA256={})", x23::hexdump(sha));
    return ctx.out();
  }
};
static_assert(std::formattable<hogd::ContractBlob, char>);
