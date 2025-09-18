#include "hogd/hogdb/rpc_from_exec.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stop_token>
#include <sys/socket.h>
#include <thread>
#include <utility>

// generated x23 headers
#include "account.hh"
#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/debug/key_type_check.h"
#include "rpc_types.hh"
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "x23/cleanup.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

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
    return RPCError::kRPCStorageWriteFailed;
  case HogDBCode::kInternal:
  case HogDBCode::kCorruption:
  case HogDBCode::kNoMemory:
  case HogDBCode::kIO:
    return RPCError::kRPCUnrecoverable;
  default:
    return RPCError::kRPCTransactionFailed;
    break;
  }
}

std::span<const std::byte>
to_span(const Bytestring &bs)
{
  return {reinterpret_cast<const std::byte *>(bs.data()), bs.size()};
}

} // namespace

void
HogDBExecutorServer::start_server(uint16_t port)
{
  lfd_ = x23::stcplisten(port);
  if (!lfd_) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  port_number_ = x23::get_port(x23::local_addr(lfd_.get()));

  listen_thread_ = std::jthread([this](std::stop_token &&stop) {
    while (!stop.stop_requested()) {

      sockaddr_storage ss;
      socklen_t sslen = sizeof(ss);
      x23::UniqueFd fd{
          accept(lfd_.get(), reinterpret_cast<sockaddr *>(&ss), &sslen)};
      if (!fd) {
        info("accept error: {}", Errno{});
      }
      else {
        x23::srpc_stream_logged(fd.release())->server(this);
      }
    }
  });
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
                                            from_xdr(delta.nnint_add())));
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    return to_rpc_error(tx.request_nnint128(make_item_key(addrkey),
                                            from_xdr(delta.nnint().set_value),
                                            from_xdr(delta.nnint().add_value)));
  case StorageDeltaType::kDeltaTypeDelete:
    return to_rpc_error(tx.request_deletion(make_item_key(addrkey)));
  case StorageDeltaType::kDeltaTypeDeployContract:
    assert(addrkey.key.key_type() ==
           StorageKeyType::kStorageKeyTypeAddressMetadata);
    return to_rpc_error(
        tx.request_deploy_contract(addrkey.address,
                                   delta.deployed_contract_hash()));
  default:
    return RPCError::kRPCUnimplemented;
  }
}

RPCError
HogDBExecutorServer::commit_transaction(BlockID block, TransactionID tx, NewContractBatch new_contracts)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCBadBlockID;

  // NOLINTNEXTLINE
  auto write_contract = [this] (Contract contract) {
    assert(contract.contract_type() == ContractType::kWasmi);
    auto data = x23::serialize(contract);
    const auto *cptr = reinterpret_cast<const std::byte *>(data.data());
    auto buf = std::span<const std::byte>(cptr, data.size());
    SHA256Hash hash = sha256(buf);
    return ctx_.get_db().write_wasm_code(hash, buf);
  };

  for (auto& contract: new_contracts) {
    // write contract should be infallible
    if (auto r = write_contract(std::move(contract)); !r.has_value()) {
      return RPCError::kRPCUnrecoverable;
    }
  }
  // TODO(geoff): write contracts here
  itx->commit();
  ctx_.close_invoke_tx(block, tx);

  return RPCError::kRPCNone;
}

RPCError
HogDBExecutorServer::abort_transaction(BlockID block, TransactionID tx)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCBadBlockID;
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
    return RPCError::kRPCBadBlockID;
  int depth = itx->create_savepoint();
  for (auto const &delta : batch) {
    if constexpr (is_debug_build()) {
      check_delta_type(delta.addrkey.key.key_type(), delta.delta.type());
    }
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
    return RPCError::kRPCBadBlockID;

  itx->rollback_to_savepoint(itx->get_savepoint_head() - popcount);

  return RPCError::kRPCNone;
}

ExpectedStorageItem
HogDBExecutorServer::get_snapshot_item(BlockID /*unused*/,
                                       AddressAndKey const &addrkey)
{
  // TODO(abelay): BlockID is ignored because we can only read the latest
  // snapshot state. In theory we could instead support different snapshot
  // heads for each BlockID, but the database is not currently designed to do
  // this.

  ItemKey key = make_item_key(addrkey);
  if (key.is_address_balance()) {
    HogDBStatus<int128_t> nnint =
        ctx_.get_db().read_address_balance(key.get_address());
    if (!nnint)
      return ExpectedStorageItem(to_rpc_error(nnint.error()));
    StorageItem out(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint() = to_xdr(*nnint);
    ExpectedStorageItem ret;
    ret.error(RPCError::kRPCNone);
    ret.item() = out;
    return ret;
  }

  HogDBStatus<DBItem> item = ctx_.get_db().read_contract_data_item(key);
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

ExpectedContract
HogDBExecutorServer::get_contract(BlockID /*block*/, SHA256Hash const &hash)
{
  // TODO(abelay): BlockID is ignored here too for the same reasons.

  HogDBStatus<OwningSpan<const std::byte>> wasm =
      ctx_.get_db().read_wasm_code(hash);
  if (!wasm)
    return ExpectedContract(to_rpc_error(wasm.error()));

  ExpectedContract out;
  out.error(RPCError::kRPCNone);

  x23::deserialize(&out.contract(), x23::BufView{*wasm});
  return out;
}

ExpectedDeployment
HogDBExecutorServer::get_deployment(BlockID /*unused*/,
                                    ContractAddress const &contract_address)
{

  HogDBStatus<DeployedContractHash> hash =
      ctx_.get_db().read_address_wasm_hash(contract_address);
  if (!hash)
    return ExpectedDeployment(to_rpc_error(hash.error()));

  ExpectedDeployment out;
  out.error(RPCError::kRPCNone);
  out.deployed_hash() = hash->hash;
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
