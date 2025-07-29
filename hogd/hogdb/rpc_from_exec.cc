#include "hogd/hogdb/rpc_from_exec.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

// generated x23 headers
#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/rpc/rpc.h"

namespace hogd {

namespace {

constexpr RPCError
to_rpc_error(HogDBStatus<void> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDBStatus; the current ones don't translate well.
  if (ret)
    return RPCError::kRPCNone;

  switch (ret.error().code()) {
  case HogDBCode::kNotFound:
    return RPCError::kRPCNotFound;
  case HogDBCode::kOverflow:
  case HogDBCode::kConflict:
    return RPCError::kRPCTransactionFailed;
  default:
    return RPCError::kRPCStorageWriteFailed;
  }
}

ItemKey
make_item_key(const AddressAndKey &addr)
{
  if (addr.key.key_type() == StorageKeyType::kStorageKeyTypeContractData)
    return ItemKey(addr.address, {}, true);
  return ItemKey(addr.address, addr.key.contract_data_key());
}

std::span<const std::byte>
to_span(const Bytestring &bs)
{
  return {reinterpret_cast<const std::byte *>(bs.data()), bs.size()};
}

} // namespace

ErrnoStatus<void>
HogDBExecutorServer::start_server(uint16_t port)
{
  ErrnoStatus<std::shared_ptr<RPCServer>> server = RPCServer::create(
      port,
      [this](std::span<const std::byte> args,
             std::string_view peer) -> OwningSpan<const std::byte> {
        return hogd_from_execd_handler(*this, args, peer);
      });
  if (!server)
    return server.error();
  rpc_server_ = std::move(*server);
  rpc_listener_th_ = std::jthread([this] {
    if (ErrnoStatus<void> ret = rpc_server_->listen(); !ret)
      error("Database to executor server listen failed: {}", ret.error());
  });
  return {};
}

// HogDBExecutorServer implementations

RPCError
HogDBExecutorServer::request_one(InvokeTX &tx, AddressAndKey const &addrkey,
                                 StorageDelta const &delta,
                                 StorageItemRPCFlagSet /*flags*/)
{
  // TODO(abelay): support StorageItemRPCFlagSet, which could request exclusive.
  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return to_rpc_error(
        tx.request_bytes(make_item_key(addrkey), to_span(delta.bytestring())));
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
    return to_rpc_error(tx.request_nnint128(make_item_key(addrkey),
                                            from_xdr(delta.nnint().add_value)));
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    return to_rpc_error(tx.request_nnint128(make_item_key(addrkey),
                                            from_xdr(delta.nnint().set_value),
                                            from_xdr(delta.nnint().add_value)));
  case StorageDeltaType::kDeltaTypeDelete:
    return to_rpc_error(tx.request_deletion(make_item_key(addrkey)));
  default:
    return RPCError::kRPCUnimplemented;
  }
}

RPCError
HogDBExecutorServer::commit_transaction(BlockID block, TransactionID tx)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCNotFound;
  itx->commit();
  ctx_.close_invoke_tx(block, tx);
  return RPCError::kRPCNone;
}

RPCError
HogDBExecutorServer::abort_transaction(BlockID block, TransactionID tx)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCNotFound;
  itx->abort();
  ctx_.close_invoke_tx(block, tx);
  return RPCError::kRPCNone;
}

RPCError
HogDBExecutorServer::try_push_new_subtransaction(BlockID block,
                                                 TransactionID tx,
                                                 StorageDeltaBatch const &batch)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCNotFound;
  int depth = itx->create_savepoint();

  for (auto const &delta : batch) {
    if (RPCError err =
            request_one(*itx, delta.addrkey, delta.delta, delta.flags);
        err != RPCError::kRPCNone) {
      itx->rollback_to_savepoint(depth - 1);
      return err;
    }
  }

  return RPCError::kRPCNone;
}

RPCError
HogDBExecutorServer::pop_subtransactions(BlockID block, TransactionID tx,
                                         uint32_t popcount)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCNotFound;

  itx->release_savepoint(itx->get_savepoint_head() - popcount - 1);
  return RPCError::kRPCNone;
}

ExpectedStorageItem
HogDBExecutorServer::get_snapshot_item(BlockID /*block*/,
                                       AddressAndKey const &addrkey)
{
  // TODO(abelay): BlockID is ignored because we can only read the latest
  // snapshot state. In theory we could instead support different snapshot
  // heads for each BlockID, but the database is not currently designed to do
  // this.

  ItemKey key = make_item_key(addrkey);
  if (key.is_address_balance()) {
    HogDBStatus<int128_t> nnint =
        read_address_balance(ctx_.get_db(), key.get_address());
    if (!nnint)
      return ExpectedStorageItem(to_rpc_error(nnint.error()));
    StorageItem out(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint() = to_xdr(*nnint);
    ExpectedStorageItem ret;
    ret.error(RPCError::kRPCNone);
    ret.item() = out;
    return ret;
  }

  HogDBStatus<DBItem> item = ctx_.get_db().get(key.as_bytes());
  if (!item)
    return ExpectedStorageItem(to_rpc_error(item.error()));

  StorageItem out;
  switch (item->type) {
  case kNNInt128:
  {
    out.type(StorageItemType::kItemTypeNonnegativeInt);
    int128_t nnint;
    std::ranges::copy(item->value, writable_byte_view(nnint).begin());
    out.nnint() = to_xdr(nnint);
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
  default:
    return ExpectedStorageItem(to_rpc_error(HogDBError(HogDBCode::kInternal)));
  }

  ExpectedStorageItem ret;
  ret.error(RPCError::kRPCNone);
  ret.item() = out;
  return ret;
}

RPCError
HogDBExecutorServer::commit_contract_create(BlockID /*block*/,
                                            Contract const &contract)
{
  // TODO(abelay): this must be included as part of BlockTX, not just written.

  const auto &wasm = contract.wasmi_contract();
  const auto *cptr = reinterpret_cast<const std::byte *>(wasm.data());
  auto buf = std::span<const std::byte>(cptr, wasm.size());
  SHA256Hash hash = sha256(buf);
  return to_rpc_error(write_wasm_code(ctx_.get_db(), hash, buf));
}

RPCError
HogDBExecutorServer::commit_contract_deploy(BlockID /*block*/,
                                            ContractAddress const &addr,
                                            SHA256Hash const &hash)
{
  // TODO(abelay): this must be included as part of BlockTX, not just written.

  return to_rpc_error(write_address_wasm_hash(ctx_.get_db(), addr, hash));
}

ExpectedContract
HogDBExecutorServer::get_contract(BlockID /*block*/, SHA256Hash const &hash)
{
  // TODO(abelay): BlockID is ignored here too for the same reasons.

  HogDBStatus<OwningSpan<const std::byte>> wasm =
      read_wasm_code(ctx_.get_db(), hash);
  if (!wasm)
    return ExpectedContract(to_rpc_error(wasm.error()));

  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract();
  const auto *cptr = reinterpret_cast<const char *>(wasm->data());
  contract.wasmi_contract().append_range(
      std::span<const char>(cptr, wasm->size()));

  ExpectedContract out;
  out.error(RPCError::kRPCNone);
  out.contract() = contract;
  return out;
}

ExpectedDeployment
HogDBExecutorServer::get_deployment(BlockID /*block*/,
                                    ContractAddress const &contract_address)
{
  // TODO(abelay): BlockID is ignored here too for the same reasons.

  HogDBStatus<SHA256Hash> hash =
      read_address_wasm_hash(ctx_.get_db(), contract_address);
  if (!hash)
    return ExpectedDeployment(to_rpc_error(hash.error()));

  ExpectedDeployment out;
  out.error(RPCError::kRPCNone);
  out.deployed_hash() = *hash;
  return out;
}

ExpectedRetrocausalQueueBlock
HogDBExecutorServer::get_next_retrocausal_queue_block(
    BlockID /*block*/, AddressAndKey const & /*addrkey*/,
    uint64_t /*search_start_number*/)
{
  return ExpectedRetrocausalQueueBlock(RPCError::kRPCUnimplemented);
}

ExpectedQueueEntry
HogDBExecutorServer::get_concurrent_retrocausal_queue_entry(
    BlockID /*block*/, AddressAndKey const & /*addrkey*/, uint32_t /*index*/)
{
  return ExpectedQueueEntry(RPCError::kRPCUnimplemented);
}

} // namespace hogd
