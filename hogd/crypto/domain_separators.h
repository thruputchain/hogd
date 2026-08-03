#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "auth_claim.hh"
#include "hogd/base/cpputil.h"
#include "hogd/crypto/crypto.h"
#include "preimage.hh"
#include "x23/serialize_cxs.h"

namespace hogd {

// A string literal usable as a non-type template parameter, backed by
// `char8_t`. We deliberately avoid the ordinary-`char` `StringLiteral`: the
// bytes below are hashed into a transaction-auth address, so they must be the
// standard's UTF-8 encoding rather than implementation-defined ordinary-literal
// code units, which depend on the execution character set (`-fexec-charset`)
// and could diverge across honest builds. See hogd issues #2573 and #2579 for
// the same charset-independence fix applied elsewhere.
template<std::size_t N> struct Utf8StringLiteral {
  char8_t buf[N];
  consteval Utf8StringLiteral(const char8_t (&str)[N])
  {
    std::copy_n(str, N, buf);
  }
};
template<size_t N>
Utf8StringLiteral(const char8_t (&)[N]) -> Utf8StringLiteral<N>;

template<size_t N>
consteval DomainSeparator
make_domain_separator(const char8_t (&name)[N])
{
  return {
      .ext = ExtensionPoint(0),
      .domain_separator = x23::Vec<std::uint8_t, 256>(name, name + N - 1),
  };
}

// CXS-encode a DomainSeparator built from Name. Array size is
// cxs_size of that concrete value (not a type-level bound).
template<Utf8StringLiteral Name>
consteval auto
domain_separator_cxs()
{
  return x23::serialize_cxs_array<x23::cxs_size(
      make_domain_separator(Name.buf))>(make_domain_separator(Name.buf));
}

constexpr static auto kPubkeyAuthDomainSeparatorCxs =
    domain_separator_cxs<u8"PubkeyAccount">();

// Derives the address of a standalone-pubkey account:
// SHA256(domain_separator || cxs(preimage)). Returns std::nullopt if the
// preimage cannot be CXS-serialized (e.g. an unrecognized pubkey scheme),
// so callers on the (guest-reachable) auth-verification path can reject the
// claim instead of aborting the process.
inline std::optional<ContractAddress>
pubkey_auth_address(PubkeyAccountPreimage const &preimage)
{
  auto serialized = x23::serialize_cxs(preimage);
  if (!serialized.has_value()) {
    return std::nullopt;
  }
  SHA256Hasher hasher;
  hasher.update(kPubkeyAuthDomainSeparatorCxs);
  hasher.update(*serialized);
  ContractAddress address = hasher.finalize();
  return address;
}

} // namespace hogd
