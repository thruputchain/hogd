#include "hogd/hog_db/rpc_from_exec.h"

#include <cassert>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "hogd/hog_exec/common/block_id_util.h"

// generated x23 headers
#include "address_auth.hh"
#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/debug/delta_validity_checks.h"
#include "hogd/debug/key_type_check.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/perthread_arc.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/invoke_tx.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db_status.h"
#include "x23/async.h"
#include "x23/format.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr RPCError
to_rpc_error(HogDbStatus<void> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDbStatus; the current ones don't translate well.
  if (ret)
    return RPCError::kRPCNone;

  switch (ret.error().code()) {
  case HogDbCode::kNotFound:
    return RPCError::kRPCNotFound;
  case HogDbCode::kOverflow:
  case HogDbCode::kConflict:
    return RPCError::kRPCStorageWriteFailed;
  case HogDbCode::kTooBig:
    // A staged item exceeds RAMDB's size limit (e.g. an oversized
    // bytestring); fail this transaction rather than the whole block.
    // Contract code rows no longer produce kTooBig: they are chunked, and
    // rows above the serialized bound are kInvalidExecutorBehavior (#2569).
    return RPCError::kRPCTransactionFailed;
  case HogDbCode::kStaleBlock:
    return RPCError::kRPCBadBlockID;
  case HogDbCode::kInvalidExecutorBehavior:
  case HogDbCode::kInternal:
  case HogDbCode::kCorruption:
  case HogDbCode::kNoMemory:
  case HogDbCode::kIO:
    error("Unrecoverable: received Hogdb error: {}", ret.error().raw_code());
    return RPCError::kRPCUnrecoverable;
  default:
    error("Received unhandled error type from RPC: {}", ret.error().raw_code());
    return RPCError::kRPCTransactionFailed;
    break;
  }
}

std::span<const std::byte>
to_span(const Bytestring &bs)
{
  return {reinterpret_cast<const std::byte *>(bs.data()), bs.size()};
}

// Diagnostic level for a push, selected from the block's execution mode:
// proposals report conflicts (everything in debug builds, first-only in
// release); validation and viewing report nothing.
constexpr DbDiagnosticLevel
diagnostic_level_for(BlockExecutionMode mode)
{
  if (mode != BlockExecutionMode::kBlockExecutionModeProposal) {
    return DbDiagnosticLevel::kDbDiagnosticNone;
  }
  return is_debug_build() ? DbDiagnosticLevel::kDbDiagnosticAll
                          : DbDiagnosticLevel::kDbDiagnosticFirst;
}

} // namespace

// HogDbExecutorRpc implementations

RPCError
HogDbExecutorRpc::request_one(InvokeTX &tx, AddressAndKey const &addrkey,
                              StorageDelta &&delta, StorageDeltaFlagSet flags,
                              DbConflictCode *conflict_code)
{
  auto finish = [&tx, conflict_code](HogDbStatus<void> ret) {
    RPCError err = to_rpc_error(ret);
    if (err == RPCError::kRPCStorageWriteFailed && conflict_code != nullptr) {
      *conflict_code = tx.conflict_reason_for(ret.error().code());
    }
    return err;
  };

  switch (delta.type()) {
  case StorageDeltaType::kDeltaTypeBytestringSet:
    return finish(tx.request_bytes(make_item_key(addrkey),
                                   to_span(delta.bytestring()), flags));
  case StorageDeltaType::kDeltaTypeNonnegativeIntAdd:
    return finish(tx.request_nnint128(make_item_key(addrkey),
                                      from_xdr(delta.nnint_add()), flags));
  case StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd:
    return finish(tx.request_nnint128(make_item_key(addrkey),
                                      from_xdr(delta.nnint().set_value),
                                      from_xdr(delta.nnint().add_value),
                                      flags));
  case StorageDeltaType::kDeltaTypeDelete:
    return finish(tx.request_deletion(make_item_key(addrkey), flags));
  case StorageDeltaType::kDeltaTypeDeployContract:
    assert(addrkey.key.key_type() == StorageKeyType::Metadata);
    return finish(tx.request_deploy_contract(addrkey.address,
                                             delta.deployed_contract_hash(),
                                             flags));
  case StorageDeltaType::kDeltaTypeAuthDataChange:
    assert(addrkey.key.key_type() == StorageKeyType::AuthData);
    return finish(
        tx.request_auth_data(addrkey.address, delta.auth_data_change(), flags));
  case StorageDeltaType::kDeltaTypeConsumeUniquifier:
    CHECK(addrkey.key.key_type() == StorageKeyType::ReplayCache);
    return finish(tx.request_consume_uniquifier(
        addrkey.address, delta.consume_uniquifier_args().uniquifier, flags));
  case StorageDeltaType::kDeltaTypeRetrocausalQueue:
    // delta is a batch element we own (moved in from the deserialized RPC
    // args), so move its queue delta into the move-only request API instead of
    // deep-copying the x23 vectors.
    return finish(tx.request_retrocausal_queue(
        make_item_key(addrkey), std::move(delta.retrocausal_queue()), flags));
  default:
    error("requested unhandled change type: {}", delta.type());
    return RPCError::kRPCUnimplemented;
  }
}

RPCError
HogDbExecutorRpc::commit_transaction(BlockID block, TransactionID tx,
                                     NewContractBatch new_contracts)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCBadBlockID;

  auto write_contract = [itx](Contract contract) {
    // request_new_contract content-addresses and stores the whole Contract
    // regardless of type; rv32 is the only type that is ever deployed.
    assert(contract.contract_type() == ContractType::kRv32);
    return itx->request_new_contract(std::move(contract));
  };

  for (auto &contract : new_contracts) {
    // write_contract fails if a sibling block commits and this block is
    // abandoned (kStaleBlock), or if the serialized contract exceeds
    // kMaxSerializedContractBytes (kInvalidExecutorBehavior): hog_exec
    // enforces the chain's contract size limit before execution, genesis load
    // keeps that limit under the ceiling, and storage chunks any legal row,
    // so an oversized row here means the executor is buggy.
    // Either way, abort just this transaction.
    if (auto r = write_contract(std::move(contract)); !r.has_value()) {
      CHECK(r.error().code() == HogDbCode::kStaleBlock ||
            r.error().code() == HogDbCode::kInvalidExecutorBehavior);
      itx->abort();
      ctx_.close_invoke_tx(block, tx, itx);
      // Staleness outranks the size guard: request_new_contract's preflight
      // runs before the stale-aware lock, so on an already-invalidated block
      // an oversized row would otherwise surface kRPCUnrecoverable instead
      // of the stale kRPCBadBlockID (executor Abandon) that any other
      // contract gets (#2599).
      if (ctx_.is_snapshot_read_stale(block)) {
        return RPCError::kRPCBadBlockID;
      }
      return to_rpc_error(r.error());
    }
  }

  if (ctx_.is_snapshot_read_stale(block)) {
    itx->abort();
    ctx_.close_invoke_tx(block, tx, itx);
    return RPCError::kRPCBadBlockID;
  }

  if (auto r = itx->commit(); !r.has_value()) {
    ctx_.close_invoke_tx(block, tx, itx);
    if (ctx_.is_snapshot_read_stale(block)) {
      return RPCError::kRPCBadBlockID;
    }
    return to_rpc_error(r.error());
  }
  ctx_.close_invoke_tx(block, tx, itx);

  return RPCError::kRPCNone;
}

RPCError
HogDbExecutorRpc::abort_transaction(BlockID block, TransactionID tx)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCBadBlockID;
  itx->abort();
  ctx_.close_invoke_tx(block, tx, itx);

  return RPCError::kRPCNone;
}

TryPushSubtransactionResult
HogDbExecutorRpc::try_push_new_subtransaction(BlockID block, TransactionID tx,
                                              StorageDeltaBatch batch)
{
  if constexpr (is_debug_build()) {
    trace("Trying to push new subtransaction for block {} and transaction {}",
          block, tx);
  }

  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return TryPushSubtransactionResult(RPCError::kRPCBadBlockID);
  const DbDiagnosticLevel level = diagnostic_level_for(itx->execution_mode());
  // hog_exec enforces the depth cap first, so a push past it is misbehavior.
  if (itx->get_savepoint_head() >= itx->max_savepoint_depth()) [[unlikely]] {
    return TryPushSubtransactionResult(RPCError::kRPCUnrecoverable);
  }
  int depth = itx->create_savepoint();

  if constexpr (is_debug_build()) {
    // Invariant: hog_exec sends at most one delta per key per subtransaction.
    std::set<AddressAndKey> seen_keys;
    for (auto const &delta : batch) {
      auto [_, inserted] = seen_keys.insert(delta.addrkey);
      CHECK(inserted);
    }
  }

  DbConflictResult conflict(level);
  auto record = [&](AddressAndKey const &addrkey, DbConflictCode code) {
    DbConflictDiagnostic d{.addrkey = addrkey, .code = code};
    if (level == DbDiagnosticLevel::kDbDiagnosticFirst) {
      conflict.first() = std::move(d);
    }
    else if (level == DbDiagnosticLevel::kDbDiagnosticAll) {
      conflict.conflicts().push_back(std::move(d));
    }
  };
  auto fail = [&](RPCError err) {
    itx->rollback_to_savepoint(depth - 1);
    TryPushSubtransactionResult res(err);
    if (err == RPCError::kRPCStorageWriteFailed) {
      res.conflict() = std::move(conflict);
    }
    return res;
  };

  RPCError first_err = RPCError::kRPCNone;
  // batch is owned (moved in from the deserialized RPC args), so request_one
  // takes each delta by rvalue and moves owned payloads (e.g. RCQ x23 vectors)
  // instead of deep-copying them at the request boundary.
  for (auto &delta : batch) {
    // request_one takes the delta by rvalue and may move-consume its payload
    // (e.g. RCQ x23 vectors), so render it here -- while it is still intact --
    // for the failure trace below. Debug-only, and trace() is compiled out in
    // release, so this render never runs in a release build.
    std::string shown;
    if constexpr (is_debug_build()) {
      check_delta_type(delta.addrkey.key.key_type(), delta.delta.type());
      assert(is_valid_delta(delta.delta));
      shown = x23::show(delta);
    }
    DbConflictCode conflict_code{};
    RPCError err = request_one(*itx, delta.addrkey, std::move(delta.delta),
                               delta.flags, &conflict_code);
    if (err == RPCError::kRPCNone) {
      continue;
    }
    if constexpr (is_debug_build()) {
      trace("Failed to request delta: {} {}", shown, err);
    }
    if (err != RPCError::kRPCStorageWriteFailed) {
      // Non-conflict failure (stale block, executor misbehavior, ...):
      // abort immediately in every mode, no diagnostics attached.
      return fail(err);
    }
    record(delta.addrkey, conflict_code);
    if (level != DbDiagnosticLevel::kDbDiagnosticAll) {
      return fail(err);
    }
    // kDbDiagnosticAll: best-effort — keep evaluating the remaining
    // deltas against the surviving state; single rollback at the end.
    first_err = RPCError::kRPCStorageWriteFailed;
  }

  ContractAddress balance_conflict_addr{};
  std::vector<InvokeTX::BalanceConflict> balance_conflicts;
  const bool collect_all = (level == DbDiagnosticLevel::kDbDiagnosticAll);
  auto bal_req =
      itx->sync_balance_locks(collect_all ? nullptr : &balance_conflict_addr,
                              collect_all ? &balance_conflicts : nullptr);
  if (!bal_req) {
    if constexpr (is_debug_build()) {
      trace("Failed to acquire balance locks after batch of deltas: {}",
            bal_req.error());
    }
    RPCError err = to_rpc_error(bal_req);
    if (err != RPCError::kRPCStorageWriteFailed) {
      return fail(err);
    }
    if (collect_all) {
      for (auto const &bc : balance_conflicts) {
        record(AddressAndKey{.address = bc.addr,
                             .key = StorageKey(
                                 StorageKeyType::FreeNativeTokenBalance)},
               bc.reason);
      }
    }
    else {
      record(AddressAndKey{.address = balance_conflict_addr,
                           .key = StorageKey(
                               StorageKeyType::FreeNativeTokenBalance)},
             itx->conflict_reason_for(bal_req.error().code()));
    }
    return fail(RPCError::kRPCStorageWriteFailed);
  }
  if (first_err != RPCError::kRPCNone) {
    return fail(first_err);
  }
  itx->assert_is_valid();
  return TryPushSubtransactionResult(RPCError::kRPCNone);
}

RPCError
HogDbExecutorRpc::pop_subtransactions(BlockID block, TransactionID tx,
                                      uint32_t popcount)
{
  InvokeTX *itx = ctx_.get_invoke_tx(block, tx);
  if (!itx)
    return RPCError::kRPCBadBlockID;
  unsigned int head = itx->get_savepoint_head();
  if (head < popcount) [[unlikely]] {
    error("Failed to pop subtransactions: head < popcount: {} < {}", head,
          popcount);
    return RPCError::kRPCUnrecoverable;
  }

  itx->rollback_to_savepoint(head - popcount);

  return RPCError::kRPCNone;
}

ExpectedStorageItem
HogDbExecutorRpc::get_snapshot_item(BlockID block, AddressAndKey const &addrkey)
{
  // TODO(abelay): BlockID is ignored because we can only read the latest
  // snapshot state. In theory we could instead support different snapshot
  // heads for each BlockID, but the database is not currently designed to do
  // this.

  ItemKey key = make_item_key(addrkey);
  ExpectedStorageItem ret;

  if (key.is_address_balance()) {
    HogDbStatus<int128_t> nnint =
        ctx_.get_db().read_address_balance(key.get_address());

    if (ctx_.is_snapshot_read_stale(block)) [[unlikely]]
      return ExpectedStorageItem(RPCError::kRPCBadBlockID);

    if (!nnint)
      return ExpectedStorageItem(to_rpc_error(nnint.error()));
    WireStorageItem out(StorageItemType::kItemTypeNonnegativeInt);
    out.nnint() = to_xdr(*nnint);
    ret.error(RPCError::kRPCNone);
    ret.item() = out;
  }
  else if (key.is_address_auth_data()) {
    HogDbStatus<AddressAuthData> auth_data =
        ctx_.get_db().read_address_auth_data(key.get_address());

    if (ctx_.is_snapshot_read_stale(block)) [[unlikely]]
      return ExpectedStorageItem(RPCError::kRPCBadBlockID);

    if (!auth_data)
      return ExpectedStorageItem(to_rpc_error(auth_data.error()));
    WireStorageItem out(StorageItemType::kItemTypeAuthData);
    out.auth_data() = *auth_data;
    ret.error(RPCError::kRPCNone);
    ret.item() = out;
  }
  else {
    HogDbStatus<DBItem> item = ctx_.get_db().read_contract_data_item(key);

    if (ctx_.is_snapshot_read_stale(block)) [[unlikely]]
      return ExpectedStorageItem(RPCError::kRPCBadBlockID);

    if (!item)
      return ExpectedStorageItem(to_rpc_error(item.error()));

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
        error("failed to deserialize rcq metadata: {}", e.what());
        return ExpectedStorageItem(
            to_rpc_error(HogDbError(HogDbCode::kInternal)));
      }
      break;
    }
    default:
      return ExpectedStorageItem(
          to_rpc_error(HogDbError(HogDbCode::kInternal)));
    }

    ret.error(RPCError::kRPCNone);
    ret.item() = out;
  }

  return ret;
}

ExpectedContract
HogDbExecutorRpc::get_contract(BlockID block, SHA256Hash const &hash)
{
  // TODO(abelay): BlockID is ignored here too for the same reasons.

  HogDbStatus<OwningSpan<const std::byte>> code =
      ctx_.get_db().read_contract_code(hash);

  if (ctx_.is_snapshot_read_stale(block)) [[unlikely]]
    return ExpectedContract(RPCError::kRPCBadBlockID);

  if (!code)
    return ExpectedContract(to_rpc_error(code.error()));

  ExpectedContract out;
  out.error(RPCError::kRPCNone);

  try {
    x23::deserialize(&out.contract(), x23::BufView{*code});
  } catch (x23::XdrException &e) {
    // A persistent contract-code row whose inline payload does not decode as a
    // Contract is corrupt on-disk state. Report it as a structured corruption
    // error -- the same way the retrocausal-queue metadata path above does --
    // rather than letting the decode exception unwind across the executor RPC
    // boundary, where the syscall wrapper would translate it into an opaque
    // unrecoverable host error.
    error("failed to deserialize persistent contract row: {}", e.what());
    return ExpectedContract(to_rpc_error(HogDbError(HogDbCode::kCorruption)));
  }

  return out;
}

ExpectedDeployment
HogDbExecutorRpc::get_deployment(BlockID block,
                                 ContractAddress const &contract_address)
{
  HogDbStatus<DeployedContractHash> hash =
      ctx_.get_db().read_address_metadata(contract_address);

  if (ctx_.is_snapshot_read_stale(block)) [[unlikely]]
    return ExpectedDeployment(RPCError::kRPCBadBlockID);

  if (!hash)
    return ExpectedDeployment(to_rpc_error(hash.error()));

  ExpectedDeployment out;
  out.error(RPCError::kRPCNone);
  out.deployed_hash() = *hash;
  return out;
}

ExpectedRetrocausalQueueBlock
HogDbExecutorRpc::get_next_retrocausal_queue_block(BlockID block,
                                                   AddressAndKey const &addrkey,
                                                   uint64_t search_start_number)
{
  // TODO(stutsman): BlockID is ignored because we can only read the latest
  // snapshot state.

  HogDbStatus<RetrocausalQueueBlock> queue_block =
      ctx_.get_db().read_retrocausal_queue_block(make_item_key(addrkey),
                                                 search_start_number);

  // After the DAO read, check for staleness before returning either the result
  // or a deterministic error (like not found).
  if (ctx_.is_snapshot_read_stale(block)) [[unlikely]]
    return ExpectedRetrocausalQueueBlock(RPCError::kRPCBadBlockID);

  if (!queue_block) {
    return ExpectedRetrocausalQueueBlock(to_rpc_error(queue_block.error()));
  }

  ExpectedRetrocausalQueueBlock out;
  out.error(RPCError::kRPCNone);
  out.queue_block() = *queue_block;
  return out;
}

BorrowedQueueEntryReply
HogDbExecutorRpc::get_concurrent_retrocausal_queue_entry(
    BlockID block, AddressAndKey const &addrkey, uint32_t index)
{
  PerThreadARC<BlockTX> *btx = ctx_.get_block_tx(block);
  // The reply owns the reference from here rather than a scope guard: x23
  // writes the borrowed bytes after this frame is gone, and the reply is
  // destroyed only once it has.
  BorrowedQueueEntryReply reply(ctx_, btx, block);

  if (!btx || btx->get().is_invalidated()) [[unlikely]] {
    reply.fail(RPCError::kRPCBadBlockID);
    return reply;
  }

  auto view_res =
      btx->get().get_concurrent_rcq_entry_view(make_item_key(addrkey), index);
  if (!view_res.has_value()) {
    reply.fail(to_rpc_error(view_res.error()));
    return reply;
  }
  if (!*view_res) {
    reply.fail(RPCError::kRPCNotFound);
    return reply;
  }

  reply.set_entry(**view_res);
  return reply;
}

x23::Async<ExpectedDBTip>
HogDbExecutorRpc::get_db_tip(BlockID block_id, BlockHeight block_height)
{
  // Serving is decided off the committed tip alone, keyed on the height the
  // caller opened its block with -- never on per-block snapshot state -- so
  // this cannot race the block's own creation (executor_to_db_rpc.x). Settle
  // any in-flight commit publication first: mid-publication the tip can
  // still read one below where it is about to land, the one transient way it
  // can sit under block_height - 1.
  co_await ctx_.co_wait_for_no_commit_publishing();

  HogDbStatus<DbTip> tip = ctx_.get_tip();
  if (!tip) {
    co_return ExpectedDBTip(to_rpc_error(tip.error()));
  }

  const BlockHeight tip_height = tip->value.proposal_header.block_height;
  if (tip_height >= block_height) [[unlikely]] {
    // The chain has moved to or past the caller's block: the block is dead.
    // Routine for staleness probes (the view gateway's refresh path), so not
    // logged. A sibling whose commit begins after the wait above can still
    // slip past this check; its per-transaction operations then report
    // Abandon, the same window the per-transaction tip pin used to have.
    co_return ExpectedDBTip(RPCError::kRPCBadBlockID);
  }
  if (tip_height < block_height - 1) [[unlikely]] {
    // Still below with no commit in flight: the caller opened a block whose
    // parent never committed, which dbsched_create_snapshot forbids.
    error("get_db_tip for block {} at height {}: committed tip is at {}",
          block_id, block_height, tip_height);
    co_return ExpectedDBTip(RPCError::kRPCBadBlockID);
  }

  // The tip *is* the requested block: the caller is loading a tip for the
  // committed block itself (e.g. a view opened on the tip's own id).
  // DatabaseProxy::get_db_tip also refuses this client-side; answering here
  // keeps both build types agreeing on the wire.
  if (tip->id == block_id) [[unlikely]] {
    co_return ExpectedDBTip(RPCError::kRPCBadBlockID);
  }

  ExpectedDBTip out;
  out.error(RPCError::kRPCNone);
  out.db_tip() = *tip;
  co_return out;
}

x23::Async<ExpectedViewingSnapshot>
HogDbExecutorRpc::open_viewing_snapshot()
{
  BlockID block_id = generate_block_id();
  auto res = co_await ctx_.open_viewing_snapshot(block_id);
  if (!res) {
    co_return ExpectedViewingSnapshot(to_rpc_error(res.error()));
  }

  ExpectedViewingSnapshot out;
  out.error(RPCError::kRPCNone);
  out.snapshot() = std::move(*res);
  co_return out;
}

RPCError
HogDbExecutorRpc::close_viewing_snapshot(BlockID block_id)
{
  auto *btx = ctx_.get_block_tx(block_id, false);
  if (!btx) {
    return RPCError::kRPCBadBlockID;
  }
  if (btx->initiate_shutdown()) {
    ctx_.remove_shutdown_block_tx(block_id);
  }
  return RPCError::kRPCNone;
}

} // namespace hogd
