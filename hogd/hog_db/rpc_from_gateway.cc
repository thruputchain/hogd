#include "hogd/hog_db/rpc_from_gateway.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

// generated x23 headers
#include "address_auth.hh"
#include "db_gateway.hh"
#include "network.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/version.h"
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
    return RPCError::kRPCUnrecoverable;
  }
  return RPCError::kRPCUnrecoverable;
}

// Read one item at the current committed tip.  Mirrors the logic of
// HogDbExecutorRpc::get_snapshot_item but is read-only and takes no
// BlockID (the snapshot is implicitly "latest committed").
DbGatewayItemResult
read_one_item(HogDbDAO &db, AddressAndKey const &addrkey)
{
  ItemKey key = make_item_key(addrkey);

  if (key.is_address_balance()) {
    HogDbStatus<int128_t> nnint = db.read_address_balance(key.get_address());
    if (!nnint)
      return DbGatewayItemResult(to_rpc_error(nnint.error()));
    WireStorageItem out(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint() = to_xdr(*nnint);
    DbGatewayItemResult ret;
    ret.error(RPCError::kRPCNone);
    ret.item() = out;
    return ret;
  }

  if (key.is_address_auth_data()) {
    HogDbStatus<AddressAuthData> auth_data =
        db.read_address_auth_data(key.get_address());
    if (!auth_data)
      return DbGatewayItemResult(to_rpc_error(auth_data.error()));
    WireStorageItem out(StorageItemType::kItemTypeAuthData);
    out.auth_data() = *auth_data;
    DbGatewayItemResult ret;
    ret.error(RPCError::kRPCNone);
    ret.item() = out;
    return ret;
  }

  HogDbStatus<DBItem> item = db.read_contract_data_item(key);
  if (!item)
    return DbGatewayItemResult(to_rpc_error(item.error()));

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
  {
    out.type(StorageItemType::kItemTypeBytestring);
    const auto *cptr = reinterpret_cast<const char *>(item->value.data());
    out.bytestring().append_range(
        std::span<const char>(cptr, item->value.size()));
    break;
  }
  case kRetrocausalQueue:
  {
    out.type(StorageItemType::kItemTypeRetrocausalQueue);
    try {
      x23::deserialize(&out.metadata(), x23::BufViewEnd(item->value));
    } catch (x23::XdrException &e) {
      error("dbgateway: failed to deserialize rcq metadata: {}", e.what());
      return DbGatewayItemResult(
          to_rpc_error(HogDbError(HogDbCode::kInternal)));
    }
    break;
  }
  default:
    // Unknown types (e.g. kDeployedContractHash) are intentionally not
    // surfaced via this read path yet; surface as internal.
    return DbGatewayItemResult(to_rpc_error(HogDbError(HogDbCode::kInternal)));
  }

  DbGatewayItemResult ret;
  ret.error(RPCError::kRPCNone);
  ret.item() = out;
  return ret;
}

} // namespace

DbGatewayIntroOutcome
HogDbGatewayRpc::dbgateway_intro(ProtocolCommit const &protocol)
{
  if (std::string_view(protocol) !=
      std::string_view(version::kHogdProtocol())) {
    return DbGatewayIntroOutcome::DbGatewayIntroMismatchedProtocol;
  }
  return DbGatewayIntroOutcome::DbGatewayIntroSuccess;
}

DbGatewayResult
HogDbGatewayRpc::dbgateway_get_items(AddressAndKeyBatch const &batch)
{
  HogDbDAO &db = ctx_.get_db();

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
    ctx_.wait_for_no_commit_publishing();

    DbTip tip = ctx_.get_tip();

    DbGatewayPayload payload;
    payload.block_height = tip.value.proposal_header.block_height;
    payload.items.reserve(batch.size());
    for (AddressAndKey const &k : batch)
      payload.items.push_back(read_one_item(db, k));

    if (ctx_.is_committed_read_stale(tip)) [[unlikely]] {
      continue;
    }

    DbGatewayResult ret;
    ret.error(RPCError::kRPCNone);
    ret.payload() = std::move(payload);
    return ret;
  }

  error("dbgateway_get_items: committed snapshot kept moving across {} "
        "attempts; giving up",
        kMaxAttempts);
  return DbGatewayResult(RPCError::kRPCUnrecoverable);
}

} // namespace hogd
