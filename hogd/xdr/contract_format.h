// -*-C++-*-

#include <format>
#include <openssl/sha.h>

template<> struct std::formatter<hogd::ContractBlob> {
  constexpr auto parse(auto &ctx) { return ctx.begin(); }
  auto format(const hogd::ContractBlob &blob, auto &ctx) const
  {
    std::array<std::uint8_t, 32> sha;
    SHA256(blob.wasm.data(), blob.wasm.size(), sha.data());
    std::format_to(ctx.out(), "ContractBlob(SHA256={})", x23::hexdump(sha));
    return ctx.out();
  }
};
static_assert(std::formattable<hogd::ContractBlob, char>);
