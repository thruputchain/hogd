// rpc_from_sched.cc - handles RPC messages from the scheduler.

#include "hogd/hogdb/rpc_from_sched.h"

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
#include "hogd/db_shard_routing/sharding.h" // shard_of_address, kPrimaryShard
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
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "hogd/metrics/health.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/vec.h"

namespace hogd {

namespace {

template<typename T>
DbSchedResult
to_db_result(HogDBStatus<T> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDBStatus; the current ones don't capture the range of possible errors.
  if (!ret) {
    error("to_db_result error: {}", ret.error());
    return DbSchedResult::DbSchedResultError;
  }
  return DbSchedResult::DbSchedResultOK;
}

} // namespace

x23::Async<HogAgeResults>
HogDBSchedRpc::dbsched_get_hog_ages(const ContractAddressBatch &addresses)
{
  HogDBDAO &db = ctx_.get_db();
  HogAgeResults ret;
  ret.reserve(addresses.size());

  for (const ContractAddress &address : addresses) {
    HogDBStatus<HogAge> status = db.read_address_average_hog_age(address);
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
HogDBSchedRpc::dbsched_write_hog_ages(const HogAgeBatch &ages)
{
  HogDBDAO &db = ctx_.get_db();
  for (const HogAge &age : ages) {
    if (!db.write_address_average_hog_age(age)) {
      co_return DbSchedResult::DbSchedResultError;
    }
  }
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbAddressAuthDataBatchResult>
HogDBSchedRpc::dbsched_get_address_auth_data(BlockID block_id,
                                             ContractAddressBatch accounts)
{
  HogDBDAO &db = ctx_.get_db();

  // Check that [block_id] is the last committed block observed by hogdb. We do
  // this check to ensure that the scheduler and hogdb are in sync with each
  // other.
  HogDBStatus<DbTip> tip = db.read_dbtip();
  if (!tip || tip->id != block_id) {
    co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
  }
  const BlockHeight committed_height = tip->value.proposal_header.block_height;

  DbAddressAuthDataBatchResult ret(DbSchedResult::DbSchedResultOK);
  ret.address_auth_data_batch().reserve(accounts.size());

  for (const ContractAddress &account : accounts) {
    HogDBStatus<AddressAuthData> status = db.read_address_auth_data(account);
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

HogDBStatus<void>
HogDBSchedRpc::commit_block(BlockID id, ConsensusValue value,
                            PerThreadARC<BlockTX> *btx,
                            const StorageLongHash &db_hash)
{
  // check if already committed.
  if (ctx_.get_tip().id == id) {
    error("tried to commit the same block '{}' more than once", id);
    return HogDBError(HogDBCode::kExists);
  }

  assert(btx != nullptr);

  if (btx->get().is_invalidated()) {
    error("tried to commit already-invalidated block '{}'", id);
    return HogDBError(HogDBCode::kInternal);
  }

  if (!btx->initiate_shutdown()) {
    error("Started to commit block '{}' while there were still outstanding "
          "references",
          id);
    return HogDBError(HogDBCode::kInternal);
  }
  x23::Defer _d([&] { ctx_.remove_shutdown_block_tx(id); });

  // Invalidate siblings BEFORE updating the dbtip below. The publication guard
  // also blocks new snapshots from opening after this invalidation until the
  // tip has been updated to match the newly committed data.
  // Sibling invalidation is not rolled back on commit failure; if commit()
  // fails the DB state is unrecoverable and all open blocks are invalid.
  {
    RPCContext::CommitPublicationGuard publication_guard(ctx_, id);

    HogDBStatus<void> ret = btx->get().commit();
    if (!ret) {
      // Cleanup the block tx if commit failed.
      return ret.error();
    }

    if (!ctx_.get_db().check_invariants()) {
      error("invariants check failed!!!");
      // TODO(geoff): Figure out if there's a way to rollback the block tx,
      // in ramdb; otherwise, the state is in some kind of corrupted version
      // where some core invariant is violated.
      return HogDBError(HogDBCode::kInternal);
    }

    ret = ctx_.update_dbtip(id, std::move(value), db_hash);
    if (!ret)
      return ret.error();
  }

  // persist everything to the ramdb.
  HogDBStatus<uint64_t> lsn = ctx_.get_db().commit();
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
HogDBSchedRpc::dbsched_create_snapshot(BlockID parent_id, BlockID new_id,
                                       BlockExecutionMode mode)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_open(parent_id, new_id);
  }
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  auto res = ctx_.open_block_tx_at_parent(parent_id, new_id, mode);
  if (!res && res.error() == HogDBCode::kStaleBlock) {
    error("parent_id {} is not the last committed block", parent_id);
    co_return DbSchedResult::DbSchedResultInvalidParentId;
  }
  co_return to_db_result(res);
}

x23::Async<DbBlockResult>
HogDBSchedRpc::dbsched_commit_snapshot(BlockID id, ConsensusValue value)
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

  HogDBStatus<void> ret = commit_block(id, value, btx, *db_hash);
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
HogDBSchedRpc::dbsched_abandon_snapshot(BlockID id)
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
HogDBSchedRpc::dbsched_close_snapshot(BlockID id) // NOLINT
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
HogDBSchedRpc::dbsched_load_genesis_configuration(
    GenesisConfiguration genesis_configuration, ConsensusValue value)
{
  constexpr static BlockID kGenesisBlockID = 0;
  if (ctx_.get_tip().value.proposal_header.block_height != -1) {
    error("Genesis block can only be loaded if the database has no blocks");
    co_return DbSchedResult::DbSchedResultError;
  }

  ctx_.initialize_cost_model(
      StorageCostModel(genesis_configuration.storage_cost_model));
  {
    auto r = ctx_.get_db().write_cost_model(ctx_.get_cost_model());
    if (!r.has_value()) {
      error("failed to write genesis block cost model: {}", r.error());
      co_return DbSchedResult::DbSchedResultError;
    }
  }

  // Genesis is loaded on every shard, but each shard must write only the items
  // it owns, so its lthash covers each item exactly once and the shards' hashes
  // sum to the value an unsharded db computes (the header carries that global
  // sum; see the two-pass hashing below). Ownership is by shard_of_address,
  // except content-addressed contracts, which live on the primary shard.
  ShardIdentity const &ident = ctx_.get_shard_identity();
  auto owns_address = [&](ContractAddress const &addr) {
    return shard_of_address(addr, ident.total_shards) == ident.shard_id;
  };
  bool const owns_deploy_address =
      owns_address(GenesisConfigurationLoader::kDeployContractAddress);
  bool const owns_contract_code = (kPrimaryShard == ident.shard_id);

  // The initial token supply's hog age is keyed by kDeployContractAddress, so
  // only that address's owner shard keeps the row. (Hog ages are not in the db
  // hash, so this does not affect the block hash -- but the row must not be
  // replicated onto non-owner shards.)
  if (owns_deploy_address) {
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
  }

  // Genesis runs after direct db writes in tests (for example wasm
  // code/metadata/balance initialization in the mempool test infra). We need to
  // account for these writes in the db hash. Thus, we compute the db hash with
  // those items alone in the db, then we use this hash as the base hash when
  // calling close() below.
  StorageLongHash base_hash = ctx_.get_db().compute_full_db_hash();

  // Applies the genesis items to `gitx`. With owned_only, writes just this
  // shard's items; otherwise writes all of them (to derive the global hash).
  // Returns true on failure. Walks the config from scratch each call.
  auto apply_genesis_items = [&](auto *gitx, bool owned_only) -> bool {
    bool failure = false;
    GenesisConfigurationLoader loader(genesis_configuration);
    loader.load_genesis_configuration(
        [&](StorageKey const &key, int128_t value) -> int128_t {
          // Storage items are keyed by kDeployContractAddress.
          if (owned_only && !owns_deploy_address) {
            return 0;
          }
          auto item_key = make_item_key(AddressAndKey{
              .address = GenesisConfigurationLoader::kDeployContractAddress,
              .key = key});
          auto r = gitx->request_nnint128(
              item_key, value, std::to_underlying(StorageDeltaFlags::None));
          if (!r.has_value()) {
            error("failed to write genesis block storage: {}", r.error());
            failure = true;
          }
          // invoketx handles the storage costs
          return 0;
        },
        [&](auto const &contract) {
          if (owned_only && !owns_contract_code) {
            return;
          }
          auto r = gitx->request_new_contract(*contract);
          if (!r.has_value()) {
            error("failed to write genesis block contract: {}", r.error());
            failure = true;
          }
        },
        [&](ContractAddress const &addr, DeployedContractHash const &hash) {
          if (owned_only && !owns_address(addr)) {
            return 0;
          }
          auto r = gitx->request_deploy_contract(addr, hash);
          if (!r.has_value()) {
            error("failed to write genesis block deployment: {}", r.error());
            failure = true;
          }
          return 0;
        },
        [&](ContractAddress const &addr,
            AddressAuthData const &auth_data) -> int128_t {
          if (owned_only && !owns_address(addr)) {
            return 0;
          }
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
          auto r = gitx->request_auth_data(
              addr, auth_data_change,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r.has_value()) {
            error("failed to write genesis block auth data: {}", r.error());
            failure = true;
          }
          return 0;
        });
    return failure;
  };

  // Pass 1: apply ALL items to a throwaway block tx to derive the GLOBAL block
  // hash -- the value an unsharded db computes, so every shard (any N) produces
  // the same block hash. close() only stages an in-memory changeset and this tx
  // is never committed, so the ramdb is left untouched.
  StorageLongHash global_hash;
  {
    constexpr BlockTXID kScratchBlockID =
        std::numeric_limits<BlockTXID>::max() - 1;
    auto gr = ctx_.open_block_tx(
        kScratchBlockID, BlockExecutionMode::kBlockExecutionModeProposal);
    if (!gr.has_value()) {
      error("failed to open genesis global-hash block tx: {}", gr.error());
      co_return DbSchedResult::DbSchedResultError;
    }
    auto *gitx = ctx_.get_invoke_tx(kScratchBlockID, 0);
    gitx->create_savepoint();
    if (apply_genesis_items(gitx, /*owned_only=*/false)) {
      co_return DbSchedResult::DbSchedResultError;
    }
    if (auto sr = gitx->sync_balance_locks(); !sr) {
      error("failed to sync balance locks (global pass): {}", sr.error());
      co_return DbSchedResult::DbSchedResultError;
    }
    if (auto cr = gitx->commit(); !cr) {
      error("failed to commit invoke tx (global pass): {}", cr.error());
      co_return DbSchedResult::DbSchedResultError;
    }
    auto gh = gr.value()->get().close(base_hash);
    if (!gh) [[unlikely]] {
      error("genesis global-hash close failed: {}", gh.error());
      co_return DbSchedResult::DbSchedResultError;
    }
    global_hash = *gh;
    ctx_.close_invoke_tx(kScratchBlockID, 0, gitx);
    // Discard without committing (mirrors commit_block's teardown).
    if (!gr.value()->initiate_shutdown()) {
      error("genesis global-hash block tx had outstanding references");
      co_return DbSchedResult::DbSchedResultError;
    }
    ctx_.remove_shutdown_block_tx(kScratchBlockID);
  }

  // Pass 2: apply only THIS shard's items to the real genesis block tx; its
  // close() gives this shard's PARTIAL hash (persisted in the DbTip). The
  // additive lthash makes the shards' partials sum to the global hash above.
  auto r = ctx_.open_block_tx(kGenesisBlockID,
                              BlockExecutionMode::kBlockExecutionModeProposal);
  if (!r.has_value()) {
    error("failed to open block tx: {}", r.error());
    co_return DbSchedResult::DbSchedResultError;
  }

  auto *itx = ctx_.get_invoke_tx(kGenesisBlockID, 0);
  assert(itx);
  int depth = itx->create_savepoint();

  if (apply_genesis_items(itx, /*owned_only=*/true)) {
    error("genesis block initialization failed");
    itx->rollback_to_savepoint(depth - 1);
    co_return DbSchedResult::DbSchedResultError;
  }

  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_id_tip(kGenesisBlockID);
  }

  if (auto sr = itx->sync_balance_locks(); !sr) {
    error("failed to sync balance locks: {}", sr.error());
    co_return DbSchedResult::DbSchedResultError;
  }
  if (auto cr = itx->commit(); !cr) {
    error("failed to commit invoke tx: {}", cr.error());
    co_return DbSchedResult::DbSchedResultError;
  }
  auto partial_hash_result = r.value()->get().close(base_hash);
  if (!partial_hash_result) [[unlikely]] {
    error("genesis close failed: {}", partial_hash_result.error());
    co_return DbSchedResult::DbSchedResultError;
  }
  StorageLongHash partial_hash = *partial_hash_result;
  ctx_.close_invoke_tx(kGenesisBlockID, 0, itx);

  // The header carries the GLOBAL hash (identical on every shard, whatever N);
  // the shard commits and persists its own PARTIAL hash in the DbTip.
  value.proposal_header.storage_hash =
      sha256(std::as_bytes(std::span(global_hash)));
  auto ret = to_db_result(
      commit_block(kGenesisBlockID, value, r.value(), partial_hash));
  if (ret == DbSchedResult::DbSchedResultOK) {
    co_await Health::instance().set_current_block_height(
        ctx_.get_tip().value.proposal_header.block_height);
  }
  // The committed ramdb holds only this shard's owned items, so a full recompute
  // must equal the PARTIAL hash we persisted.
  StorageLongHash full_hash = ctx_.get_db().compute_full_db_hash();
  CHECK_EQ(full_hash, partial_hash);
  co_return ret;
}

x23::Async<DbShardResult>
HogDBSchedRpc::dbsched_shard_restore(BlockHeight height)
{
  co_return DbShardResult::ShardOK;
}

} // namespace hogd
