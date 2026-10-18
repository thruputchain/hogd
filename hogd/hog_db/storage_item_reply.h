// -*-C++-*-

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

// generated x23 headers
#include "rpc_types.hh"
#include "storage_item.hh"

#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

// A reply standing in for [Expected], a union that carries a WireStorageItem
// on kRPCNone and nothing on any other RPCError (ExpectedStorageItem,
// DbGatewayItemResult).  A bytestring row's value already is the Bytestring
// the reply carries, so the reply holds the bytes read out of the database
// and x23 writes them straight into the message, rather than copying them
// into a WireStorageItem first.  Every other item travels as the [Expected]
// it is.
template<typename Expected> class StorageItemReply {
public:
  explicit StorageItemReply(Expected item) noexcept
    : reply_(std::in_place_index<0>, std::move(item))
  {}
  explicit StorageItemReply(RPCError error) : StorageItemReply(Expected(error))
  {}

  // A kRPCNone reply carrying [bytes] as a kItemTypeBytestring item.
  static StorageItemReply bytestring(OwningSpan<const std::byte> bytes) noexcept
  {
    // The caller rejects longer rows, which no Bytestring can carry.
    CHECK(bytes.size() <= kBytestringCeilingLen);
    return StorageItemReply(std::move(bytes));
  }

  // The bytestring this reply carries, or null if it carries an [Expected].
  const OwningSpan<const std::byte> *bytestring() const noexcept
  {
    return std::get_if<1>(&reply_);
  }
  // The [Expected] this reply carries, or null if it carries a bytestring.
  const Expected *item() const noexcept { return std::get_if<0>(&reply_); }

  RPCError error() const noexcept
  {
    return bytestring() ? RPCError::kRPCNone : item()->error();
  }

private:
  explicit StorageItemReply(OwningSpan<const std::byte> bytes) noexcept
    : reply_(std::in_place_index<1>, std::move(bytes))
  {}

  std::variant<Expected, OwningSpan<const std::byte>> reply_;
};

// A bytestring goes out as x23 writes the generated unions: the RPCError
// discriminant, the StorageItemType discriminant, then the opaque.
template<typename Expected>
size_t
xdr_size(const StorageItemReply<Expected> &reply)
{
  if (const auto *bytes = reply.bytestring()) {
    return 8 + x23::xdr_size(x23::BufView(*bytes));
  }
  return x23::xdr_size(*reply.item());
}

template<typename Expected>
void
xdr_serialize(auto &serializer, const StorageItemReply<Expected> &reply)
{
  if (const auto *bytes = reply.bytestring()) {
    serializer.put32(static_cast<uint32_t>(RPCError::kRPCNone));
    serializer.put32(
        static_cast<uint32_t>(StorageItemType::kItemTypeBytestring));
    serializer(x23::BufView(*bytes));
    return;
  }
  serializer(*reply.item());
}

} // namespace hogd

namespace x23 {
// Declares StorageItemReply<Expected> a stand-in for the Expected an RPC
// interface declares.
template<typename Expected>
struct serialize_equiv<Expected, hogd::StorageItemReply<Expected>>
  : std::true_type {};
} // namespace x23
