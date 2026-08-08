// rpc_from_sched.cc - handles RPC messages from the scheduler.

#include "hogd/hog_db/rpc_from_sched.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <sys/socket.h>
#include <utility>

// generated x23 headers
#include "address_auth.hh"
#include "auth_claim.hh"
#include "consensus.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "genesis.hh"

#include "hogd/base/compiler.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/string.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/hex_array.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/block_tx.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/status.h"
#include "hogd/metrics/health.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/vec.h"

namespace hogd {

namespace {

template<typename T>
DbSchedResult
to_db_result(HogDbStatus<T> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDbStatus; the current ones don't capture the range of possible errors.
  if (!ret) {
    error("to_db_result error: {}", ret.error());
    return DbSchedResult::DbSchedResultError;
  }
  return DbSchedResult::DbSchedResultOK;
}

} // namespace

x23::Async<HogAgeResults>
HogDbSchedRpc::dbsched_get_hog_ages(const ContractAddressBatch &addresses)
{
  HogDbDAO &db = ctx_.get_db();
  HogAgeResults ret;
  ret.reserve(addresses.size());

  for (const ContractAddress &address : addresses) {
    HogDbStatus<HogAge> status = db.read_address_average_hog_age(address);
    if (!status) {
      warn("Failed to read hog age for address {}: {}", HexArray(address).str(),
           status.error());
      ret.push_back(HogAgeResult(DbSchedResult::DbSchedResultError));
      continue;
    }
    ret.push_back(HogAgeResult(DbSchedResult::DbSchedResultOK));
    ret.back().hog_age() = status.value();
  }
  co_return ret;
}

x23::Async<DbSchedResult>
HogDbSchedRpc::dbsched_write_hog_ages(const HogAgeBatch &ages)
{
  HogDbDAO &db = ctx_.get_db();
  for (const HogAge &age : ages) {
    if (!db.write_address_average_hog_age(age)) {
      co_return DbSchedResult::DbSchedResultError;
    }
  }
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbAddressAuthDataBatchResult>
HogDbSchedRpc::dbsched_get_address_auth_data(BlockID block_id,
                                             ContractAddressBatch accounts)
{
  HogDbDAO &db = ctx_.get_db();

  // Check that [block_id] is the last committed block observed by hog_db. We do
  // this check to ensure that the scheduler and hog_db are in sync with each
  // other.
  HogDbStatus<DbTip> tip = db.read_dbtip();
  if (!tip || tip->id != block_id) {
    co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
  }
  const BlockHeight committed_height = tip->value.proposal_header.block_height;

  DbAddressAuthDataBatchResult ret(DbSchedResult::DbSchedResultOK);
  ret.address_auth_data_batch().reserve(accounts.size());

  for (const ContractAddress &account : accounts) {
    HogDbStatus<AddressAuthData> status = db.read_address_auth_data(account);
    if (!status) {
      co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
    }
    // The dbtip id guard above has the same commit-window race as the AuthDB
    // height check: BlockTX::commit() applies the block to the live DAO before
    // update_dbtip() writes the new tip, so a row already written by the next
    // block can be read here while read_dbtip() still reports [block_id].
    // Reject if the row is newer than the committed tip we validated against
    // (issue #1725).
    if (status.value().last_written_height > committed_height) {
      co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
    }
    ret.address_auth_data_batch().push_back(status.value());
  }
  co_return ret;
}

HogDbStatus<void>
HogDbSchedRpc::commit_block(BlockID id, ConsensusValue value,
                            PerThreadARC<BlockTX> *btx,
                            const StorageLongHash &db_hash)
{
  // check if already committed.
  if (ctx_.get_tip().id == id) {
    error("tried to commit the same block '{}' more than once", id);
    return HogDbError(HogDbCode::kExists);
  }

  assert(btx != nullptr);

  if (btx->get().is_invalidated()) {
    error("tried to commit already-invalidated block '{}'", id);
    return HogDbError(HogDbCode::kInternal);
  }

  if (!btx->initiate_shutdown()) {
    error("Started to commit block '{}' while there were still outstanding "
          "references",
          id);
    return HogDbError(HogDbCode::kInternal);
  }
  x23::Defer _d([&] { ctx_.remove_shutdown_block_tx(id); });

  // Invalidate siblings BEFORE updating the dbtip below. The publication guard
  // also blocks new snapshots from opening after this invalidation until the
  // tip has been updated to match the newly committed data.
  // Sibling invalidation is not rolled back on commit failure; if commit()
  // fails the DB state is unrecoverable and all open blocks are invalid.
  {
    RPCContext::CommitPublicationGuard publication_guard(ctx_, id);

    HogDbStatus<void> ret = btx->get().commit();
    if (!ret) {
      // Cleanup the block tx if commit failed.
      return ret.error();
    }

    if (!ctx_.get_db().check_invariants()) {
      error("invariants check failed!!!");
      // TODO(geoff): Figure out if there's a way to rollback the block tx,
      // in ramdb; otherwise, the state is in some kind of corrupted version
      // where some core invariant is violated.
      return HogDbError(HogDbCode::kInternal);
    }

    ret = ctx_.update_dbtip(id, std::move(value), db_hash);
    if (!ret)
      return ret.error();
  }

  // persist everything to the ramdb.
  HogDbStatus<uint64_t> lsn = ctx_.get_db().commit();
  if (!lsn)
    return lsn.error();

  DbTip committed_tip = ctx_.get_tip();
  auto committed_txs_hash = committed_tip.value.proposal_header.txs_hash;
  auto committed_events_hash = committed_tip.value.proposal_header.events_hash;
  info("committed block {}: txs_hash='{}' events_hash='{}', db_hash='{}', "
       "height='{}', lsn='{}",
       id, encode_base64(std::as_bytes(std::span(committed_txs_hash))),
       encode_base64(std::as_bytes(std::span(committed_events_hash))),
       encode_base64(std::as_bytes(std::span(committed_tip.storage_long_hash))),
       committed_tip.value.proposal_header.block_height, *lsn);
  CHECK_NE(committed_tip.value.proposal_header.block_height, -1);
  return {};
}

x23::Async<DbSchedResult>
HogDbSchedRpc::dbsched_create_snapshot(BlockID parent_id, BlockID new_id,
                                       BlockExecutionMode mode)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_open(parent_id, new_id);
  }
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  auto res = ctx_.open_block_tx_at_parent(parent_id, new_id, mode);
  if (!res && res.error() == HogDbCode::kStaleBlock) {
    error("parent_id {} is not the last committed block", parent_id);
    co_return DbSchedResult::DbSchedResultInvalidParentId;
  }
  co_return to_db_result(res);
}

x23::Async<DbBlockResult>
HogDbSchedRpc::dbsched_commit_snapshot(BlockID id, ConsensusValue value)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_commit(id);
    ctx_.get_db().enable_debug_invariants(ctx_.get_cost_model());
  }
  x23::Defer _d([&] { ctx_.get_db().clear_invariants(); });

  auto *btx = ctx_.get_block_tx(id, false);
  if (btx == nullptr) [[unlikely]]
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);

  std::optional<StorageLongHash> db_hash = ctx_.take_db_hash(id);
  if (!db_hash.has_value()) {
    error("dbsched_commit_snapshot called before close for block {}", id);
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);
  }

  HogDbStatus<void> ret = commit_block(id, value, btx, *db_hash);
  if (!ret)
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);

  co_await Health::instance().set_current_block_height(
      ctx_.get_tip().value.proposal_header.block_height);

  DbBlockResult result(DbSchedResult::DbSchedResultOK);
  DbTip tip = ctx_.get_tip();
  result.block().storage_hash = tip.value.proposal_header.storage_hash;
  result.block().storage_long_hash = tip.storage_long_hash;
  co_return result;
}

x23::Async<DbSchedResult>
HogDbSchedRpc::dbsched_abandon_snapshot(BlockID id)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_abandon(id);
  }
  auto *ret = ctx_.get_block_tx(id, false);
  if (!ret) {
    co_return DbSchedResult::DbSchedResultError;
  }

  if (ret->initiate_shutdown()) {
    // remove_shutdown_block_tx() also drops any cached db hash for this block.
    ctx_.remove_shutdown_block_tx(id);
  }

  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbBlockResult>
HogDbSchedRpc::dbsched_close_snapshot(BlockID id) // NOLINT
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_close(id);
  }
  // TODO(abelay): rename this to finish_snapshot. It will be called before
  // either commit_snapshot or abort_snapshot because the db hash is needed
  // to create a proposal block.

  // TODO(abelay): we could potential free memory here too.
  PerThreadARC<BlockTX> *btx = ctx_.get_block_tx(id, false);
  if (btx == nullptr) [[unlikely]] {
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);
  }

  auto db_hash_result =
      btx->get().close(/*base_hash=*/ctx_.get_tip().storage_long_hash);
  if (!db_hash_result) [[unlikely]] {
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);
  }
  DbBlockResult ret(DbSchedResult::DbSchedResultOK);
  ctx_.cache_db_hash(id, *db_hash_result);
  ret.block().storage_hash = sha256(std::as_bytes(std::span(*db_hash_result)));
  ret.block().storage_long_hash = *db_hash_result;
  co_return ret;
}

namespace {

template<typename T> requires std::is_trivially_copyable_v<T>
inline ErrnoStatus<void>
sys_getsockname(int sockfd, T &val)
{
  socklen_t len = sizeof(val); // NOLINT(misc-include-cleaner)
  return make_errno_status<void>(
      getsockname(sockfd, reinterpret_cast<sockaddr *>(&val), &len));
}

} // namespace

x23::Async<DbSchedResult>
HogDbSchedRpc::dbsched_load_genesis_configuration(
    GenesisConfiguration genesis_configuration, ConsensusValue value)
{
  constexpr static BlockID kGenesisBlockID = 0;
  if (ctx_.get_tip().value.proposal_header.block_height != -1) {
    error("Genesis block can only be loaded if the database has no blocks");
    co_return DbSchedResult::DbSchedResultError;
  }

  if (genesis_configuration.default_wallet.hash == SHA256Hash{}) {
    error("genesis configuration has a zero default_wallet contract hash");
    co_return DbSchedResult::DbSchedResultError;
  }
  ctx_.set_default_wallet(genesis_configuration.default_wallet);

  ctx_.initialize_cost_model(
      StorageCostModel(genesis_configuration.storage_cost_model));
  {
    auto r = ctx_.get_db().write_cost_model(ctx_.get_cost_model());
    if (!r.has_value()) {
      error("failed to write genesis block cost model: {}", r.error());
      co_return DbSchedResult::DbSchedResultError;
    }
  }

  uint128_t initial_hog_token_supply =
      GenesisConfigurationLoader::initial_hog_token_supply();
  HogAge ages{
      .address = GenesisConfigurationLoader::kDeployContractAddress,
      .num_hogs = {to_xdr(initial_hog_token_supply), to_xdr(uint128_t{0}),
                   to_xdr(uint128_t{0})},
      .average_ages = {BlockHeight{0}, BlockHeight{0}, BlockHeight{0}},
  };
  if (auto age_write = ctx_.get_db().write_address_average_hog_age(ages);
      !age_write.has_value()) {
    error("failed to write genesis block hog ages: {}", age_write.error());
    co_return DbSchedResult::DbSchedResultError;
  }

  // Genesis runs after direct db writes in tests (for example wasm
  // code/metadata/balance initialization in the mempool test infra). We need to
  // account for these writes in the db hash. Thus, we compute the db hash with
  // those items alone in the db, then we use this hash as the base hash when
  // calling close() below.
  StorageLongHash base_hash = ctx_.get_db().compute_full_db_hash();

  auto r = ctx_.open_block_tx(kGenesisBlockID,
                              BlockExecutionMode::kBlockExecutionModeProposal);
  if (!r.has_value()) {
    error("failed to open block tx: {}", r.error());
    co_return DbSchedResult::DbSchedResultError;
  }

  auto *itx = ctx_.get_invoke_tx(kGenesisBlockID, 0);
  int depth = itx->create_savepoint();
  assert(itx);

  bool failure = false;
  // If initialization fails, then the database is left in an invalid state.
  // We should abort the entire run of the hog chain to fix if initialization
  // doesn't work.

  GenesisConfigurationLoader loader(genesis_configuration);
  loader.load_genesis_configuration(
      [&](StorageKey const &key, int128_t value) -> int128_t {
        auto item_key = make_item_key(AddressAndKey{
            .address = GenesisConfigurationLoader::kDeployContractAddress,
            .key = key});
        auto r =
            itx->request_nnint128(item_key, value,
                                  std::to_underlying(StorageDeltaFlags::None));
        if (!r.has_value()) {
          error("failed to write genesis block storage: {}", r.error());
          failure = true;
        }

        // invoketx handles the storage costs
        return 0;
      },
      [&](auto const &contract) {
        auto r = itx->request_new_contract(*contract);
        if (!r.has_value()) {
          error("failed to write genesis block contract: {}", r.error());
          failure = true;
        }
      },
      [&](ContractAddress const &addr, DeployedContractHash const &hash) {
        auto r = itx->request_deploy_contract(addr, hash);
        if (!r.has_value()) {
          error("failed to write genesis block deployment: {}", r.error());
          failure = true;
        }
        return 0;
      },
      [&](ContractAddress const &addr,
          AddressAuthData const &auth_data) -> int128_t {
        x23::Vec<AuthDataChangeSigner, kMaxSigners> auth_data_change_signers;
        for (uint32_t i = 0; i < kMaxSigners; i++) {
          if (auth_data.public_keys[i]) {
            auth_data_change_signers.push_back({
                .public_key_id = i,
                .public_key = auth_data.public_keys[i],
            });
          }
        }

        AuthDataChangeArgs auth_data_change = {
            .auth_data_change_signers = auth_data_change_signers,
            .new_required_weight = auth_data.required_weight,
        };

        auto r =
            itx->request_auth_data(addr, auth_data_change,
                                   std::to_underlying(StorageDeltaFlags::None));
        if (!r.has_value()) {
          error("failed to write genesis block auth data: {}", r.error());
          failure = true;
        }
        return 0;
      });

  if (failure) {
    error("genesis block initialization failed");
    itx->rollback_to_savepoint(depth - 1);
    co_return DbSchedResult::DbSchedResultError;
  }

  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_id_tip(kGenesisBlockID);
  }

  if (auto r = itx->sync_balance_locks(); !r) {
    error("failed to sync balance locks: {}", r.error());
    co_return DbSchedResult::DbSchedResultError;
  }
  if (auto cr = itx->commit(); !cr) {
    error("failed to commit invoke tx: {}", cr.error());
    co_return DbSchedResult::DbSchedResultError;
  }
  auto db_hash_result = r.value()->get().close(base_hash);
  if (!db_hash_result) [[unlikely]] {
    error("genesis close failed: {}", db_hash_result.error());
    co_return DbSchedResult::DbSchedResultError;
  }
  StorageLongHash db_hash = *db_hash_result;
  ctx_.close_invoke_tx(kGenesisBlockID, 0, itx);

  value.proposal_header.storage_hash =
      sha256(std::as_bytes(std::span(db_hash)));
  auto ret =
      to_db_result(commit_block(kGenesisBlockID, value, r.value(), db_hash));
  if (ret == DbSchedResult::DbSchedResultOK) {
    co_await Health::instance().set_current_block_height(
        ctx_.get_tip().value.proposal_header.block_height);
  }
  // Just to be safe, we recompute the db hash from scratch and ensure that
  // [db_hash] is equal to it.
  StorageLongHash full_hash = ctx_.get_db().compute_full_db_hash();
  CHECK_EQ(full_hash, db_hash);
  co_return ret;
}

x23::Async<DbShardResult>
HogDbSchedRpc::dbsched_shard_restore(BlockHeight height)
{
  co_return DbShardResult::ShardOK;
}

} // namespace hogd
