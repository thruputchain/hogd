// -*-C++-*-

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

// generated x23 headers
#include "db_gateway.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/storage_item_reply.h"
#include "x23/serialize.h"

namespace hogd {

// Own the database buffer until serialization instead of copying it into
// the generated response's opaque vector.
class DbGatewayContractReply {
public:
  explicit DbGatewayContractReply(RPCError error) noexcept : error_(error)
  {
    CHECK(error_ != RPCError::kRPCNone);
  }
  explicit DbGatewayContractReply(OwningSpan<const std::byte> bytes) noexcept
    : error_(RPCError::kRPCNone), bytes_(std::move(bytes))
  {}

  RPCError error() const noexcept { return error_; }
  std::span<const std::byte> contract_xdr() const noexcept { return bytes_; }

private:
  RPCError error_;
  OwningSpan<const std::byte> bytes_;
};

inline size_t
xdr_size(const DbGatewayContractReply &reply)
{
  return 4 + (reply.error() == RPCError::kRPCNone
                  ? x23::xdr_size(x23::BufView(reply.contract_xdr()))
                  : 0);
}

inline void
xdr_serialize(auto &serializer, const DbGatewayContractReply &reply)
{
  serializer.put32(static_cast<uint32_t>(reply.error()));
  if (reply.error() == RPCError::kRPCNone) {
    serializer(x23::BufView(reply.contract_xdr()));
  }
}

// Reply for dbgateway_get_items, standing in for the DbGatewayResult the RPC
// interface declares.  Its items are StorageItemReplys, so a bytestring goes
// from the row read out of ramdb straight into the message.
class DbGatewayItemsReply {
public:
  using Item = StorageItemReply<DbGatewayItemResult>;

  // A reply that carries no payload.
  explicit DbGatewayItemsReply(RPCError error) noexcept : error_(error)
  {
    CHECK(error_ != RPCError::kRPCNone);
  }

  // A kRPCNone reply carrying [items], read at [block_height].
  DbGatewayItemsReply(BlockHeight block_height,
                      std::vector<Item> items) noexcept
    : error_(RPCError::kRPCNone), block_height_(block_height),
      items_(std::move(items))
  {}

  RPCError error() const noexcept { return error_; }
  BlockHeight block_height() const noexcept { return block_height_; }
  std::span<const Item> items() const noexcept { return items_; }

private:
  RPCError error_;
  BlockHeight block_height_ = 0;
  std::vector<Item> items_;
};

// As x23 writes the generated union: discriminant, then on kRPCNone the
// DbGatewayPayload's height and its length-prefixed items.
inline size_t
xdr_size(const DbGatewayItemsReply &reply)
{
  size_t size = 4; // discriminant
  if (reply.error() == RPCError::kRPCNone) {
    size += x23::xdr_size(reply.block_height()) + 4; // height, item count
    for (auto const &item : reply.items()) {
      size += xdr_size(item);
    }
  }
  return size;
}

inline void
xdr_serialize(auto &serializer, const DbGatewayItemsReply &reply)
{
  serializer.put32(static_cast<uint32_t>(reply.error()));
  if (reply.error() != RPCError::kRPCNone) {
    return;
  }
  serializer(reply.block_height());
  serializer.put32(static_cast<uint32_t>(reply.items().size()));
  for (auto const &item : reply.items()) {
    serializer(item);
  }
}

} // namespace hogd

namespace x23 {
template<>
struct serialize_equiv<hogd::DbGatewayContractResult,
                       hogd::DbGatewayContractReply> : std::true_type {};

// Declares DbGatewayItemsReply a stand-in for the DbGatewayResult the
// DbGateway1 interface declares.  Must precede the is_DbGateway1 assertion
// below, which is what checks it.
template<>
struct serialize_equiv<hogd::DbGatewayResult, hogd::DbGatewayItemsReply>
  : std::true_type {};
} // namespace x23

namespace hogd {

// Read-only x23 RPC server exposed from Db (hog_db) to the public RPC
// gateway (see protocol/db_gateway.x for the XDR).  Serves
// committed-snapshot reads.
//
// Item batches and configuration readings each carry their committed snapshot.
class HogDbGatewayRpc {
public:
  using rpc_service = DbGateway1;

  explicit HogDbGatewayRpc(RPCContext &ctx) : ctx_(ctx) {}

  // Caps on a single dbgateway_get_items batch, rejected outright when
  // exceeded: kMaxBatchItems bounds the read count and result-vector size,
  // kMaxResponseBytes the cumulative serialized size (items vary in size).
  static constexpr std::size_t kMaxBatchItems = 16384;
  static constexpr std::size_t kMaxResponseBytes = 15UL * 1024 * 1024;

  static DbGatewayIntroOutcome dbgateway_intro(ProtocolCommit const &protocol);

  // A coroutine: it may suspend to wait out a commit publication, and the
  // rpc server runs handlers on ThreadPool workers, which must not park.
  // Own the decoded batch because RPC dispatch releases its arguments when
  // a handler suspends.
  x23::Async<DbGatewayItemsReply> dbgateway_get_items(AddressAndKeyBatch batch);

  x23::Async<DbGatewayConfigResult> dbgateway_get_config();

  DbGatewayContractReply dbgateway_get_contract(SHA256Hash const &hash);

  void shut_down() { shutdown_flag_ = true; }

  void await_shutdown_request() { shutdown_flag_.wait(false); }

private:
  std::atomic<bool> shutdown_flag_ = false;
  RPCContext &ctx_;
};

// MaybeAsync rather than type_identity_t: dbgateway_get_items is a coroutine
// (it can suspend on a commit publication), which is also the transform
// RpcServer itself validates.
static_assert(is_DbGateway1<HogDbGatewayRpc *, x23::MaybeAsync>);

class HogDbGatewayServer : public RpcServer<HogDbGatewayRpc> {
public:
  explicit HogDbGatewayServer(RPCContext &ctx) : RpcServer(ctx) {}
};

} // namespace hogd
