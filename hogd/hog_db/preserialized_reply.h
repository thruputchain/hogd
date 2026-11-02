// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

// generated x23 headers
#include "rpc_types.hh"

#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "x23/serialize.h"

namespace hogd {

// An RPC reply standing in for [Expected], a union that carries one value on
// kRPCNone and nothing on any other RPCError, when that value is already in
// its XDR encoding -- typically a database row stored in the value's wire
// format.  x23 writes the bytes out as they are, so the reply skips decoding
// them into an [Expected] only to encode them again.
//
// The caller vouches that the bytes encode the value: nothing here parses
// them, and bytes that do not decode reach the client as a malformed reply.
template<typename Expected> class PreserializedReply {
public:
  // A reply that carries no value.
  explicit PreserializedReply(RPCError error) noexcept : error_(error)
  {
    CHECK(error_ != RPCError::kRPCNone);
  }

  // A reply carrying [value], the XDR encoding of the value on kRPCNone.
  explicit PreserializedReply(OwningSpan<const std::byte> value) noexcept
    : error_(RPCError::kRPCNone), value_(std::move(value))
  {
    // XDR pads every item to a multiple of four bytes, so nothing else is
    // an encoding.
    CHECK(value_.size() % 4 == 0);
  }

  RPCError error() const noexcept { return error_; }
  std::span<const std::byte> value() const noexcept { return value_; }

private:
  RPCError error_;
  OwningSpan<const std::byte> value_;
};

// As x23 writes the generated union: discriminant, then the active arm.
template<typename Expected>
size_t
xdr_size(const PreserializedReply<Expected> &reply)
{
  return 4 + reply.value().size();
}

template<typename Expected>
void
xdr_serialize(auto &serializer, const PreserializedReply<Expected> &reply)
{
  serializer.put32(static_cast<uint32_t>(reply.error()));
  // Already a multiple of four bytes, so putbytes adds no padding.
  serializer.putbytes(reply.value().data(), reply.value().size());
}

} // namespace hogd

namespace x23 {
// Declares PreserializedReply<Expected> a stand-in for the Expected an RPC
// interface declares.
template<typename Expected>
struct serialize_equiv<Expected, hogd::PreserializedReply<Expected>>
  : std::true_type {};
} // namespace x23
