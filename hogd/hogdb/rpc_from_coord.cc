// rpc_from_coord.cc - handles RPC messages from the coordinator.

#include "hogd/hogdb/rpc_from_coord.h"

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
#include "db_coord.hh"
#include "genesis.hh"

#include "rpc_types.hh"
#include "types.hh"

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/hogdb/db.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_block.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/status.h"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

ItemKey
make_item_key(const AddressAndKey &addr)
{
  if (addr.key.key_type() ==
      StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)
    return ItemKey(addr.address, {}, true);
  return ItemKey(addr.address, addr.key.contract_data_key());
}

template<typename T>
DbResult
to_db_result(HogDBStatus<T> ret)
{
  // TODO(abelay): the RPC error codes ought to be the same as or similar to
  // HogDBStatus; the current ones don't capture the range of possible errors.
  if (!ret)
    return DbResult::DbResultError;
  return DbResult::DbResultOK;
}

} // namespace

x23::Async<DbResult>
HogDBCoordServer::dbcoord_create_snapshot(BlockID parent_id, BlockID new_id,
                                          BlockExecutionMode mode)
{
  // TODO(abelay): the parent_id must be last committed snapshot. We could
  // check that here if we had the block height rather than the ID.
  co_return to_db_result(ctx_.open_block_tx(new_id));
}

x23::Async<DbBlockResult>
HogDBCoordServer::dbcoord_commit_snapshot(BlockID id)
{
  BlockTX *btx = ctx_.get_block_tx(id);
  if (!btx) {
    co_return DbBlockResult(DbResult::DbResultError);
  }

  DbBlockResult ret(to_db_result(btx->commit()));
  ctx_.get_trie().generate_top_level_hashes();
  ret.block_hash() = ctx_.get_trie().get_root_hash();
  ctx_.close_block_tx(id);
  co_return ret;
}

x23::Async<DbResult>
HogDBCoordServer::dbcoord_abandon_snapshot(BlockID id)
{
  BlockTX *ret = ctx_.get_block_tx(id);
  if (!ret)
    co_return DbResult::DbResultError;

  BlockTX &btx = *ret;
  btx.abort();
  ctx_.close_block_tx(id);
  co_return DbResult::DbResultOK;
}

x23::Async<DbBlockResult>
HogDBCoordServer::dbcoord_close_snapshot(BlockID id) // NOLINT
{
  // TODO(abelay): Get rid of this. This should not be a separate RPC method.
  // Instead both commit and abort should implicitly close.

  // ctx_.close_block_tx(id);
  co_return DbBlockResult(DbResult::DbResultOK);
}

x23::Async<HogAgeResult>
HogDBCoordServer::dbcoord_get_hog_ages(const ContractAddressBatch &addresses)
{
  HogDatabase &database = ctx_.get_db();
  HogAgeResult ret(DbResult::DbResultOK);
  ret.hog_age_batch().reserve(addresses.size());

  for (const ContractAddress &address : addresses) {
    HogDBStatus<HogAge> status =
        read_address_average_hog_age(database, address);
    if (!status) {
      co_return HogAgeResult(DbResult::DbResultError);
    }
    // TODO(jhumphri): Include the address in each index.
    ret.hog_age_batch().push_back(status.value());
  }
  co_return ret;
}

x23::Async<DbResult>
HogDBCoordServer::dbcoord_write_hog_ages(const HogAgeBatch &ages)
{
  HogDatabase &database = ctx_.get_db();
  for (const HogAge &age : ages) {
    if (!write_address_average_hog_age(database, age)) {
      co_return DbResult::DbResultError;
    }
  }
  co_return DbResult::DbResultOK;
}

namespace {
template<typename Result, typename T>
ErrnoStatus<Result>
make_errno_status(T ret)
{
  // first check for error.
  if (ret == -1)
    return Errno{};

  // otherwise return the result type.
  assert(ret >= 0);
  if constexpr (std::is_integral_v<Result>) {
    return static_cast<Result>(ret);
  }
  else if (std::is_void_v<Result>) {
    assert(ret == 0);
    return {};
  }
  else {
    static_assert("unsupported result type");
  }
}

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
HogDBCoordServer::start_server(uint16_t port)
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

    [](x23::CoroPoll &cp, HogDBCoordServer *server,
       x23::UniqueFd lfd) -> x23::Detached {
      co_await cp.pool_.bg();
      while (auto c = co_await cp.accept(lfd)) {
        x23::rpc_stream_logged(cp, *c).server(server);
      }
    }(cp_, this, std::move(lfd));
  }
}

x23::Async<DbResult>
HogDBCoordServer::dbcoord_load_genesis_block(GenesisBlock genesis_block)
{
  if (is_initialized_) {
    co_return DbResult::DbResultError;
  }

  is_initialized_ = true;

  
  auto r = ctx_.open_block_tx(0);
  if (!r.has_value()) {
    error("failed to open block tx: {}", r.error());
    co_return DbResult::DbResultError;
  }

  auto *itx = ctx_.get_invoke_tx(0, 0);
  assert(itx);

  bool failure = false;
  // If initialization fails, then the database is left in an invalid state.
  // We should abort the entire run of the hog chain to fix if initialization doesn't work.

  GenesisBlockLoader loader(genesis_block);
  loader.load_genesis_block(
      [&](StorageKey const &key, int128_t value) {
        auto item_key = make_item_key(
            AddressAndKey{.address = GenesisBlockLoader::kDeployContractAddress,
                          .key = key});
        auto r = itx->request_nnint128(item_key, value);
        if (!r.has_value()) {
          error("failed to write genesis block storage: {}", r.error());
          failure = true;
        }
      },
      [&](auto const &contract) {
        auto data = x23::serialize(*contract);
        const auto *cptr = reinterpret_cast<const std::byte *>(data.data());
        auto buf = std::span<const std::byte>(cptr, data.size());
        SHA256Hash hash = sha256(buf);
        auto r = write_wasm_code(ctx_.get_db(), hash, buf);
        if (!r.has_value()) {
          error("failed to write genesis block contract: {}", r.error());
          failure = true;
        }
      },
      [&](ContractAddress const &addr, SHA256Hash const &hash) {
        auto r = write_address_wasm_hash(ctx_.get_db(), addr, hash);
        if (!r.has_value()) {
          error("failed to write genesis block deployment: {}", r.error());
          failure = true;
        }
      });

  if (failure) {
    error("genesis block initialization failed");
    co_return DbResult::DbResultError;
  }
  
  itx->commit();
  ctx_.close_invoke_tx(0, 0);
  auto commit_res = ctx_.get_block_tx(0)->commit();
  if (!commit_res.has_value()) {
    error("failed to commit genesis block: {}", commit_res.error());
    co_return DbResult::DbResultError;
  }



  ctx_.close_block_tx(0);

  co_return DbResult::DbResultOK;
}

uint16_t
HogDBCoordServer::port()
{
  std::unique_lock lk(mtx_);
  cv_.wait(lk, [this] { return port_.has_value(); });
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  return port_.value();
}

} // namespace hogd
