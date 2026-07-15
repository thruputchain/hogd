#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "auth_claim.hh"
#include "hogd/base/cpputil.h"
#include "hogd/crypto/crypto.h"
#include "preimage.hh"
#include "x23/serialize_cxs.h"

namespace hogd {

template<size_t N>
consteval DomainSeparator
make_domain_separator(const char (&name)[N])
{
  return {
      .ext = ExtensionPoint(0),
      .domain_separator = x23::Vec<std::uint8_t, 256>(name, name + N - 1),
  };
}

// CXS-encode a DomainSeparator built from Name. Array size is
// cxs_size of that concrete value (not a type-level bound).
template<StringLiteral Name>
consteval auto
domain_separator_cxs()
{
  return x23::serialize_cxs_array<x23::cxs_size(
      make_domain_separator(Name.buf))>(make_domain_separator(Name.buf));
}

constexpr static auto kPubkeyAuthDomainSeparatorCxs =
    domain_separator_cxs<"PubkeyAccount">();

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
