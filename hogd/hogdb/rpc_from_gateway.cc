#include "hogd/hogdb/rpc_from_gateway.h"

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
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/version.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

// Map a HogDBError to the public RPCError surface.  This is a
// READ-ONLY path: write-failure codes (`kOverflow`, `kConflict`,
// `kExists`) shouldn't fire here at all, and the read-relevant
// codes that *can* surface from a read of corrupted/oversized
// state (`kTypeMismatch`, `kTooBig`) are unrecoverable for the
// caller — they don't indicate a write was attempted and rejected.
// All non-NotFound codes therefore map to `kRPCUnrecoverable`.
constexpr RPCError
to_rpc_error(HogDBError err)
{
  switch (err.code()) {
  case HogDBCode::kNotFound:
    return RPCError::kRPCNotFound;
  case HogDBCode::kOverflow:
  case HogDBCode::kConflict:
  case HogDBCode::kExists:
  case HogDBCode::kTypeMismatch:
  case HogDBCode::kTooBig:
  case HogDBCode::kInternal:
  case HogDBCode::kCorruption:
  case HogDBCode::kNoMemory:
  case HogDBCode::kIO:
  case HogDBCode::kInvalidExecutorBehavior:
  case HogDBCode::kStaleBlock:
    return RPCError::kRPCUnrecoverable;
  }
  return RPCError::kRPCUnrecoverable;
}

// Read one item at the current committed tip.  Mirrors the logic of
// HogDBExecutorRpc::get_snapshot_item but is read-only and takes no
// BlockID (the snapshot is implicitly "latest committed").
DbGatewayItemResult
read_one_item(HogDBDAO &db, AddressAndKey const &addrkey)
{
  ItemKey key = make_item_key(addrkey);

  if (key.is_address_balance()) {
    HogDBStatus<int128_t> nnint = db.read_address_balance(key.get_address());
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
    HogDBStatus<AddressAuthData> auth_data =
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

  HogDBStatus<DBItem> item = db.read_contract_data_item(key);
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
          to_rpc_error(HogDBError(HogDBCode::kInternal)));
    }
    break;
  }
  default:
    // Unknown types (e.g. kDeployedContractHash) are intentionally not
    // surfaced via this read path yet; surface as internal.
    return DbGatewayItemResult(to_rpc_error(HogDBError(HogDBCode::kInternal)));
  }

  DbGatewayItemResult ret;
  ret.error(RPCError::kRPCNone);
  ret.item() = out;
  return ret;
}

} // namespace

DbGatewayIntroOutcome
HogDBGatewayRpc::dbgateway_intro(Protocol const &protocol)
{
  if (std::string_view(protocol) !=
      std::string_view(version::kHogdProtocol())) {
    return DbGatewayIntroOutcome::DbGatewayIntroMismatchedProtocol;
  }
  return DbGatewayIntroOutcome::DbGatewayIntroSuccess;
}

DbGatewayResult
HogDBGatewayRpc::dbgateway_get_items(AddressAndKeyBatch const &batch)
{
  HogDBDAO &db = ctx_.get_db();

  // Capture the committed height once up-front so every item in the
  // response is labeled as coming from the same snapshot.  Any commits
  // that land between now and the individual reads would bump the tip,
  // but the reads below still see the pre-commit state (ramdb applies
  // committed state atomically per block).
  BlockHeight height = ctx_.get_height_threadsafe();

  DbGatewayPayload payload;
  payload.block_height = height;
  payload.items.reserve(batch.size());
  for (AddressAndKey const &k : batch)
    payload.items.push_back(read_one_item(db, k));

  DbGatewayResult ret;
  ret.error(RPCError::kRPCNone);
  ret.payload() = std::move(payload);
  return ret;
}

} // namespace hogd
