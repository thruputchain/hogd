#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "hogd/base/cpputil.h"
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

} // namespace hogd
