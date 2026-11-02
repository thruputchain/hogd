#include "hogd/hog_db/rpc_from_gateway.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// generated x23 headers
#include "address_auth.hh"
#include "db_gateway.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"
#include "hogd/hog_db/storage_item_reply.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/version.h"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// Map a HogDbError to the public RPCError surface.  This is a
// READ-ONLY path: write-failure codes (`kOverflow`, `kConflict`,
// `kExists`) shouldn't fire here at all, and the read-relevant
// codes that *can* surface from a read of corrupted/oversized
// state (`kTypeMismatch`, `kTooBig`) are unrecoverable for the
// caller — they don't indicate a write was attempted and rejected.
// All non-NotFound codes therefore map to `kRPCUnrecoverable`.
constexpr RPCError
to_rpc_error(HogDbError err)
{
  switch (err.code()) {
  case HogDbCode::kNotFound:
    return RPCError::kRPCNotFound;
  case HogDbCode::kOverflow:
  case HogDbCode::kConflict:
  case HogDbCode::kExists:
  case HogDbCode::kTypeMismatch:
  case HogDbCode::kTooBig:
  case HogDbCode::kInternal:
  case HogDbCode::kCorruption:
  case HogDbCode::kNoMemory:
  case HogDbCode::kIO:
  case HogDbCode::kInvalidExecutorBehavior:
  case HogDbCode::kStaleBlock:
  case HogDbCode::kBusy:
    return RPCError::kRPCUnrecoverable;
  }
  return RPCError::kRPCUnrecoverable;
}

// Read one item at the current committed tip.  Mirrors the logic of
// HogDbExecutorRpc::get_snapshot_item but is read-only and takes no
// BlockID (the snapshot is implicitly "latest committed").
DbGatewayItemsReply::Item
read_one_item(HogDbDAO &db, AddressAndKey const &addrkey)
{
  using Reply = DbGatewayItemsReply::Item;
  ItemKey key = make_item_key(addrkey);

  if (key.is_address_balance()) {
    HogDbStatus<int128_t> nnint = db.read_address_balance(key.get_address());
    if (!nnint)
      return Reply(to_rpc_error(nnint.error()));
    WireStorageItem out(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint() = to_xdr(*nnint);
    DbGatewayItemResult ret;
    ret.error(RPCError::kRPCNone);
    ret.item() = std::move(out);
    return Reply(std::move(ret));
  }

  if (key.is_address_auth_data()) {
    HogDbStatus<AddressAuthData> auth_data =
        db.read_address_auth_data(key.get_address());
    if (!auth_data)
      return Reply(to_rpc_error(auth_data.error()));
    WireStorageItem out(StorageItemType::kItemTypeAuthData);
    out.auth_data() = std::move(*auth_data);
    DbGatewayItemResult ret;
    ret.error(RPCError::kRPCNone);
    ret.item() = std::move(out);
    return Reply(std::move(ret));
  }

  if (key.is_address_metadata()) {
    auto deployment = db.read_address_metadata(key.get_address());
    if (!deployment)
      return Reply(to_rpc_error(deployment.error()));
    WireStorageItem out(StorageItemType::kItemTypeDeployedContractHash);
    out.deployed_contract_hash() = std::move(*deployment);
    DbGatewayItemResult ret(RPCError::kRPCNone);
    ret.item() = std::move(out);
    return Reply(std::move(ret));
  }

  HogDbStatus<DBItem> item = db.read_contract_data_item(key);
  if (!item)
    return Reply(to_rpc_error(item.error()));

  WireStorageItem out;
  switch (item->type) {
  case kNNInt128:
  {
    out.type(StorageItemType::kItemTypeNonnegativeInt);
    CHECK(item->value.size() == sizeof(int128_xdr));
    auto encoded =
        std::span<const std::byte, sizeof(int128_xdr)>(item->value.data(),
                                                       sizeof(int128_xdr));
    out.nnint() = to_xdr(int128_from_xdr_bytes(encoded));
    break;
  }
  case kBytes:
    // The row already is the Bytestring, so the reply carries it as read.
    if (item->value.size() > kBytestringCeilingLen) [[unlikely]] {
      error("dbgateway: bytestring row of {} bytes exceeds the {} byte "
            "ceiling",
            item->value.size(), kBytestringCeilingLen);
      return Reply(to_rpc_error(HogDbError(HogDbCode::kCorruption)));
    }
    return Reply::bytestring(std::move(item->value));
  case kRetrocausalQueue:
  {
    out.type(StorageItemType::kItemTypeRetrocausalQueue);
    try {
      x23::deserialize(&out.metadata(), x23::BufViewEnd(item->value));
    } catch (x23::XdrException &e) {
      error("dbgateway: failed to deserialize rcq metadata: {}", e.what());
      return Reply(to_rpc_error(HogDbError(HogDbCode::kInternal)));
    }
    break;
  }
  default:
    // Unknown types (e.g. kDeployedContractHash) are intentionally not
    // surfaced via this read path yet; surface as internal.
    return Reply(to_rpc_error(HogDbError(HogDbCode::kInternal)));
  }

  DbGatewayItemResult ret;
  ret.error(RPCError::kRPCNone);
  ret.item() = std::move(out);
  return Reply(std::move(ret));
}

} // namespace

DbGatewayIntroOutcome
HogDbGatewayRpc::dbgateway_intro(ProtocolCommit const &protocol)
{
  if (protocol != version::kHogdProtocol()) {
    return DbGatewayIntroOutcome::DbGatewayIntroMismatchedProtocol;
  }
  return DbGatewayIntroOutcome::DbGatewayIntroSuccess;
}

x23::Async<DbGatewayConfigResult>
HogDbGatewayRpc::dbgateway_get_config()
{
  co_await ctx_.co_wait_for_no_commit_publishing();
  DbTip tip = ctx_.get_tip();
  if (tip.value.proposal_header.block_height < 0) {
    co_return DbGatewayConfigResult(RPCError::kRPCNotFound);
  }
  // Prices are immutable after genesis; limits come from the captured tip.
  // A later commit does not invalidate this configuration reading.
  auto cost_model = ctx_.get_db().read_cost_model();
  if (!cost_model) {
    co_return DbGatewayConfigResult(RPCError::kRPCUnrecoverable);
  }
  if (tip.network_config.v() != 0 ||
      !NetworkConfigParams::is_canonical(tip.network_config.v0())) {
    co_return DbGatewayConfigResult(RPCError::kRPCUnrecoverable);
  }
  DbGatewayConfigResult result(RPCError::kRPCNone);
  result.config().effective_config =
      NetworkConfigParams(tip.network_config.v0()).to_network_config();
  result.config().storage_cost_model = cost_model->to_xdr();
  result.config().tip = std::move(tip);
  co_return result;
}

DbGatewayContractResult
HogDbGatewayRpc::dbgateway_get_contract(SHA256Hash const &hash)
{
  auto encoded = ctx_.get_db().read_contract_code(hash);
  if (!encoded) {
    return DbGatewayContractResult(to_rpc_error(encoded.error()));
  }
  if (encoded->size() > kMaxResponseBytes) {
    return DbGatewayContractResult(RPCError::kRPCUnrecoverable);
  }
  DbGatewayContractResult result(RPCError::kRPCNone);
  result.contract_xdr().resize(encoded->size());
  if (encoded->size() != 0) {
    std::memcpy(result.contract_xdr().data(), encoded->data(), encoded->size());
  }
  return result;
}

x23::Async<DbGatewayItemsReply>
HogDbGatewayRpc::dbgateway_get_items(AddressAndKeyBatch batch)
{
  HogDbDAO &db = ctx_.get_db();

  // Reject oversized batches before touching the db (see kMaxBatchItems).
  if (batch.size() > kMaxBatchItems) {
    error("dbgateway_get_items: batch of {} items exceeds cap of {}; rejecting",
          batch.size(), kMaxBatchItems);
    co_return DbGatewayItemsReply(RPCError::kRPCUnrecoverable);
  }

  // Snapshot the committed tip once up-front so every item in the response
  // is labeled with the same height.  This is a TOCTOU: a block that
  // commits between this snapshot and the reads below applies its data to
  // ramdb (BlockTX::commit) *before* it advances the tip (update_dbtip), so
  // the reads can observe height-(H+1) state while `tip.height` still names
  // H.  After the reads, re-check the snapshot: if a commit landed (or is
  // mid-publication) the height label is stale, and we retry against a fresh
  // snapshot rather than surface the race.  Mirrors the staleness check in
  // HogDbExecutorRpc::get_snapshot_item.
  //
  // A commit landing inside the read window is rare and discrete, so a fresh
  // snapshot effectively always succeeds on the next attempt — more than one
  // retry should never happen in practice.  The bounded cap is purely a
  // backstop against pathological commit churn; exhausting it is reported as
  // an internal error rather than looping forever.
  constexpr int kMaxAttempts = 5;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    // If a commit is mid-publication, wait it out so the tip and ramdb data
    // are consistent before reading (rather than burning a retry attempt).
    // Suspends rather than parks: this handler runs on a ThreadPool worker.
    co_await ctx_.co_wait_for_no_commit_publishing();

    DbTip tip = ctx_.get_tip();

    std::vector<DbGatewayItemsReply::Item> items;
    items.reserve(batch.size());
    std::size_t response_bytes = 0;
    for (AddressAndKey const &k : batch) {
      DbGatewayItemsReply::Item item = read_one_item(db, k);
      // Items are variable-sized, so bound the cumulative response size too.
      response_bytes += xdr_size(item);
      if (response_bytes > kMaxResponseBytes) [[unlikely]] {
        error("dbgateway_get_items: response exceeds {} bytes; rejecting",
              kMaxResponseBytes);
        co_return DbGatewayItemsReply(RPCError::kRPCUnrecoverable);
      }
      items.push_back(std::move(item));
    }

    if (ctx_.is_committed_read_stale(tip)) [[unlikely]] {
      continue;
    }

    co_return DbGatewayItemsReply(tip.value.proposal_header.block_height,
                                  std::move(items));
  }

  error("dbgateway_get_items: committed snapshot kept moving across {} "
        "attempts; giving up",
        kMaxAttempts);
  co_return DbGatewayItemsReply(RPCError::kRPCUnrecoverable);
}

} // namespace hogd
