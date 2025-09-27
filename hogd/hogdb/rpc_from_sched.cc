// rpc_from_sched.cc - handles RPC messages from the scheduler (also known as
// the mempool).

#include "hogd/hogdb/rpc_from_sched.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

// generated x23 headers
#include "account.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/block_tx.h"

#include "rpc_types.hh"
#include "types.hh"

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

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
HogDBSchedServer::dbsched_get_hog_ages(const ContractAddressBatch &addresses)
{
  HogDBDAO &db = ctx_.get_db();
  HogAgeResult ret(DbSchedResult::DbSchedResultOK);
  ret.hog_age_batch().reserve(addresses.size());

  for (const ContractAddress &address : addresses) {
    HogDBStatus<HogAge> status = db.read_address_average_hog_age(address);
    if (!status) {
      co_return HogAgeResult(DbSchedResult::DbSchedResultError);
    }
    // TODO(jhumphri): Include the address in each index.
    ret.hog_age_batch().push_back(status.value());
  }
  co_return ret;
}

x23::Async<DbSchedResult>
HogDBSchedServer::dbsched_write_hog_ages(const HogAgeBatch &ages)
{
  HogDBDAO &db = ctx_.get_db();
  for (const HogAge &age : ages) {
    if (!db.write_address_average_hog_age(age)) {
      co_return DbSchedResult::DbSchedResultError;
    }
  }
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbAccountAuthDataBatchResult>
HogDBSchedServer::dbsched_get_account_auth_data(BlockID block_id,
                                                ContractAddressBatch accounts)
{
  HogDBDAO &db = ctx_.get_db();
  {
    // Check that [block_id] is the last committed block observed by hogdb. We
    // do this check to ensure that the scheduler and hogdb are in sync with
    // each other.
    HogDBStatus<DbTip> ret = db.read_dbtip();
    if (!ret || ret->id != block_id) {
      co_return DbAccountAuthDataBatchResult(DbSchedResult::DbSchedResultError);
    }
  }

  DbAccountAuthDataBatchResult ret(DbSchedResult::DbSchedResultOK);
  ret.account_auth_data_batch().reserve(accounts.size());

  for (const ContractAddress &account : accounts) {
    HogDBStatus<AccountAuthData> status =
        db.read_address_account_auth_data(account);
    if (!status) {
      co_return DbAccountAuthDataBatchResult(DbSchedResult::DbSchedResultError);
    }
    ret.account_auth_data_batch().push_back(status.value());
  }
  co_return ret;
}

x23::Async<DbTipResult>
HogDBSchedServer::dbsched_get_tip()
{
  DbTipResult ret(DbSchedResult::DbSchedResultOK);
  ret.db_tip() = ctx_.get_tip();
  co_return ret;
}

HogDBStatus<void>
HogDBSchedServer::commit_block(BlockID id)
{
  // check if already committed.
  if (ctx_.get_tip().id == id) {
    error("tried to commit the same block '{}' more than once", id);
    return HogDBError(HogDBCode::kExists);
  }

  // write all the changes in the block TX.
  BlockTX *btx = ctx_.get_block_tx(id);
  HogDBStatus<void> ret = btx->commit();
  if (!ret)
    return ret.error();

  if (!ctx_.get_db().check_invariants()) {
    error("invariants check failed!!!");
    // TODO(geoff): Figure out if there's a way to rollback the block tx,
    // in ramdb; otherwise, the state is in some kind of corrupted version
    // where some core invariant is violated.
    return HogDBError(HogDBCode::kInternal);
  }

  // destroy the block TX.
  ctx_.close_block_tx(id);

  // update the DbTip.
  ret = ctx_.update_dbtip(id);
  if (!ret)
    return ret.error();

  // persist everything to the ramdb.
  HogDBStatus<uint64_t> lsn = ctx_.get_db().commit();
  if (!lsn)
    return lsn.error();

  info("committed block {}: hash='{}', height='{}', lsn='{}", id,
       ctx_.get_tip().hash, ctx_.get_tip().height, *lsn);
  assert(ctx_.get_tip().height != 0);
  return {};
}

x23::Async<DbSchedResult>
HogDBSchedServer::dbsched_create_snapshot(BlockID parent_id, BlockID new_id,
                                          BlockExecutionMode mode)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_open(parent_id, new_id);
  }
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  co_return to_db_result(ctx_.open_block_tx(new_id));
}

x23::Async<DbBlockResult>
HogDBSchedServer::dbsched_commit_snapshot(BlockID id)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_commit(id);
    ctx_.get_db().enable_debug_invariants();
  }
  BlockTX *btx = ctx_.get_block_tx(id);
  if (!btx)
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);
  HogDBStatus<void> ret = commit_block(id);
  if (!ret)
    co_return DbBlockResult(DbSchedResult::DbSchedResultError);

  DbBlockResult result(DbSchedResult::DbSchedResultOK);
  result.block_hash() = ctx_.get_tip().hash;
  co_return result;
}

x23::Async<DbSchedResult>
HogDBSchedServer::dbsched_abandon_snapshot(BlockID id)
{
  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_abandon(id);
  }
  BlockTX *ret = ctx_.get_block_tx(id);
  if (!ret)
    co_return DbSchedResult::DbSchedResultError;

  BlockTX &btx = *ret;
  btx.abort();
  ctx_.close_block_tx(id);
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbBlockResult>
HogDBSchedServer::dbsched_close_snapshot(BlockID id) // NOLINT
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
  // hash is invalid. Discuss with Geoff and David about how to resolve this.
  ctx_.get_trie().generate_top_level_hashes();
  ret.block_hash() = ctx_.get_trie().get_root_hash();
  co_return ret;
}

x23::Async<DbSchedResult>
HogDBSchedServer::dbsched_load_genesis_block(GenesisBlock genesis_block)
{
  constexpr static BlockID kGenesisBlockID = 0;
  if (ctx_.get_tip().height != 0) {
    error("Genesis block can only be loaded if the database has no blocks");
    co_return DbSchedResult::DbSchedResultError;
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

  GenesisBlockLoader loader(genesis_block);
  loader.load_genesis_block(
      [&](StorageKey const &key, int128_t value) -> int128_t {
        auto item_key = make_item_key(
            AddressAndKey{.address = GenesisBlockLoader::kDeployContractAddress,
                          .key = key});
        auto r = itx->request_nnint128(item_key, value);
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
        auto r = ctx_.get_db().write_address_metadata(addr, hash);
        if (!r.has_value()) {
          error("failed to write genesis block deployment: {}", r.error());
          failure = true;
        }
      });

  if (failure) {
    error("genesis block initialization failed");
    itx->rollback_to_savepoint(depth - 1);
    co_return DbSchedResult::DbSchedResultError;
  }

  if constexpr (is_debug_build()) {
    block_id_state_manager_.log_block_id_tip(kGenesisBlockID);
  }

  itx->commit();
  ctx_.close_invoke_tx(kGenesisBlockID, 0);
  co_return to_db_result(commit_block(kGenesisBlockID));
}

x23::Async<DbShardResult>
HogDBSchedServer::dbsched_shard_restore(BlockHeight height)
{
  co_return DbShardResult::ShardOK;
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

uint16_t
get_port(int fd)
{
  sockaddr_in saddr;
  // retrieve the port number if it was assigned automatically.
  ErrnoStatus<void> ret = sys_getsockname(fd, saddr);
  if (!ret)
    throw std::runtime_error("Failed to get socket name");
  return ntohs(saddr.sin_port);
}

} // namespace

void
HogDBSchedServer::start_server(uint16_t port)
{
  {
    auto lfd = x23::tcplisten(port);
    if (!lfd) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    std::lock_guard lock(mtx_);
    port_ = get_port(lfd.get());
    cv_.notify_all();

    [](x23::CoroPoll &cp, HogDBSchedServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c)->server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

uint16_t
HogDBSchedServer::port()
{
  std::unique_lock lk(mtx_);
  cv_.wait(lk, [this] { return port_.has_value(); });
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return port_.value();
}

} // namespace hogd
