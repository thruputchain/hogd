// rpc_from_sched.cc - handles RPC messages from the scheduler.

#include "hogd/hogdb/rpc_from_sched.h"

#include <cassert>
#include <cstddef>
#include <limits>
#include <span>
#include <sys/socket.h>
#include <utility>

// generated x23 headers
#include "address_auth.hh"
#include "auth_claim.hh"
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
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/vec.h"
#include "x23/x23.h"

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

x23::Async<HogAgeResult>
HogDBSchedRpc::dbsched_get_hog_ages(const ContractAddressBatch &addresses)
{
  HogDBDAO &db = ctx_.get_db();
  HogAgeResult ret(DbSchedResult::DbSchedResultOK);
  ret.hog_age_batch().reserve(addresses.size());

  for (const ContractAddress &address : addresses) {
    HogDBStatus<HogAge> status = db.read_address_average_hog_age(address);
    if (!status) {
      warn("Failed to read hog age for address {}: {}", HexArray(address).str(),
           status.error());
      co_return HogAgeResult(DbSchedResult::DbSchedResultError);
    }
    // TODO(jhumphri): Include the address in each index.
    ret.hog_age_batch().push_back(status.value());
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
  {
    // Check that [block_id] is the last committed block observed by hogdb. We
    // do this check to ensure that the scheduler and hogdb are in sync with
    // each other.
    HogDBStatus<DbTip> ret = db.read_dbtip();
    if (!ret || ret->id != block_id) {
      co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
    }
  }

  DbAddressAuthDataBatchResult ret(DbSchedResult::DbSchedResultOK);
  ret.address_auth_data_batch().reserve(accounts.size());

  for (const ContractAddress &account : accounts) {
    HogDBStatus<AddressAuthData> status = db.read_address_auth_data(account);
    if (!status) {
      co_return DbAddressAuthDataBatchResult(DbSchedResult::DbSchedResultError);
    }
    ret.address_auth_data_batch().push_back(status.value());
  }
  co_return ret;
}

HogDBStatus<void>
HogDBSchedRpc::commit_block(BlockID id, BlockHash block_hash,
                            PerThreadARC<BlockTX> *btx)
{
  // check if already committed.
  if (ctx_.get_tip().id == id) {
    error("tried to commit the same block '{}' more than once", id);
    return HogDBError(HogDBCode::kExists);
  }

  assert(btx != nullptr);

  if (!btx->initiate_shutdown()) {
    error("Started to commit block '{}' while there were still outstanding "
          "references",
          id);
    return HogDBError(HogDBCode::kInternal);
  }

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

  // Releases the reference to the blocktx acquried above
  // destroy the block TX.
  ctx_.remove_shutdown_block_tx(id);

  // update the DbTip.
  ret = ctx_.update_dbtip(id, block_hash);
  if (!ret)
    return ret.error();

  // persist everything to the ramdb.
  HogDBStatus<uint64_t> lsn = ctx_.get_db().commit();
  if (!lsn)
    return lsn.error();

  info("committed block {}: block_hash='{}' db_hash='{}', height='{}', lsn='{}",
       id, encode_base64(std::as_bytes(std::span(ctx_.get_tip().block_hash))),
       encode_base64(std::as_bytes(std::span(ctx_.get_tip().db_hash))),
       ctx_.get_tip().height, *lsn);
  assert(ctx_.get_tip().height != 0);
  return {};
}

x23::Async<DbSchedResult>
HogDBSchedRpc::dbsched_create_snapshot(BlockID parent_id, BlockID new_id,
                                       BlockExecutionMode mode)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_open(parent_id, new_id);
  }
  if (parent_id != ctx_.get_tip().id) {
    error("parent_id {} is not the last committed block {}", parent_id,
          ctx_.get_tip().id);
    // TODO(jhumphri): This should be an error, but we need to fix the mempool
    // tests first.
    // See hogd issue #610
    // co_return DbSchedResult::DbSchedResultError;
  }
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  co_return to_db_result(ctx_.open_block_tx(new_id));
}

x23::Async<DbBlockResult>
HogDBSchedRpc::dbsched_commit_snapshot(BlockID id, BlockHash block_hash)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_commit(id);
    ctx_.get_db().enable_debug_invariants(ctx_.get_cost_model());
  }
  x23::Defer _d([&] { ctx_.get_db().clear_invariants(); });

  auto *btx = ctx_.get_block_tx(id, false);
  if (btx == nullptr) [[unlikely]]
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);

  HogDBStatus<void> ret = commit_block(id, block_hash, btx);
  if (!ret)
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);

  DbBlockResult result(DbSchedResult::DbSchedResultOK);
  result.db_hash() = ctx_.get_tip().db_hash;
  co_return result;
}

x23::Async<DbSchedResult>
HogDBSchedRpc::dbsched_abandon_snapshot(BlockID id)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_abandon(id);
  }
  auto *ret = ctx_.get_block_tx(id, false);
  if (!ret)
    co_return DbSchedResult::DbSchedResultError;

  if (ret->initiate_shutdown()) {
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
  // either commit_snapshot or abort_snapshot because the merkle root is needed
  // to create a proposal block.

  // TODO(abelay): we could potential free memory here too.

  DbBlockResult ret(DbSchedResult::DbSchedResultOK);
  // TODO(jhumphri): We have not committed the [ItemChange]s yet, so this root
  // hash is invalid. We could add the [ItemChanges] to the Merkle Trie and
  // compute the root hash, but if the block ends up being abandoned rather than
  // committed, it is expensive to remove these items from the Merkle Trie.
  //
  // We are going to replace the Merkle Trie with a much cheaper commutative
  // hash function, so once we do that, we *can* compute the hash here with the
  // pending [ItemChanges]. If we the block is abandoned, it is cheap to revert
  // the hash.
  // ctx_.get_trie().generate_top_level_hashes();
  ret.db_hash() = ctx_.get_trie().get_root_hash();
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
    GenesisConfiguration genesis_configuration, BlockHash block_hash)
{
  constexpr static BlockID kGenesisBlockID = 0;
  if (ctx_.get_tip().height != 0) {
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

  auto r = ctx_.open_block_tx(kGenesisBlockID);
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
        auto r = itx->request_nnint128(
            item_key, value,
            std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
        if (!r.has_value()) {
          error("failed to write genesis block storage: {}", r.error());
          failure = true;
        }

        // invoketx handles the storage costs
        return 0;
      },
      [&](auto const &contract) {
        auto data = x23::serialize(*contract);
        const auto *cptr = reinterpret_cast<const std::byte *>(data.data());
        auto buf = std::span<const std::byte>(cptr, data.size());
        SHA256Hash hash = sha256(buf);
        auto r = ctx_.get_db().write_wasm_code(hash, buf);
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
        x23::Vec<AuthDataChangeSigner, std::numeric_limits<uint32_t>::max()>
            auth_data_change_signers;
        for (uint32_t i = 0; i < kMaxSigners; i++) {
          if (auth_data.public_keys[i]) {
            auth_data_change_signers.push_back({
                .public_key_id = i,
                .public_key = auth_data.public_keys[i],
            });
          }
        }

        GasConfigChange gas_config_change{};
        for (auto const &gas_permission : auth_data.gas_permissions) {
          gas_config_change.new_gas_permission_configs.push_back(
              gas_permission);
        }

        AuthDataChangeArgs auth_data_change = {
            .auth_data_change_signers = auth_data_change_signers,
            .new_nonce_config = auth_data.nonce_config,
            .new_gas_config_change =
                x23::Ptr<GasConfigChange>(gas_config_change),
        };

        auto r = itx->request_auth_data(
            addr, auth_data_change,
            std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
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
  itx->commit();
  ctx_.close_invoke_tx(kGenesisBlockID, 0, itx);

  uint128_t initial_hog_token_supply =
      GenesisConfigurationLoader::initial_hog_token_supply();

  // Initialize the genesis address with scheduler state
  CHECK(
      ctx_.get_db()
          .write_address_average_hog_age(HogAge{
              .address = GenesisConfigurationLoader::kDeployContractAddress,
              .num_hogs = {to_xdr(initial_hog_token_supply),
                           to_xdr(uint128_t{0}), to_xdr(uint128_t{0})},
              .average_ages = {BlockHeight{0}, BlockHeight{0}, BlockHeight{0}},
          })
          .has_value());

  co_return to_db_result(commit_block(kGenesisBlockID, block_hash, r.value()));
}

x23::Async<DbShardResult>
HogDBSchedRpc::dbsched_shard_restore(BlockHeight height)
{
  co_return DbShardResult::ShardOK;
}

} // namespace hogd
