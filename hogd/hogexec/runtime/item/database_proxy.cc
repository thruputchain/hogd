#include "hogd/hogexec/runtime/item/database_proxy.h"

#include <cassert>
#include <concepts>
#include <format>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "address_auth.hh"
#include "contract.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/debug/key_type_check.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/borrowed_contract_batch.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "sched_exec.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/cpumap.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {

namespace {

struct VoidMarker {};
struct NotFoundIsUnrecoverableError {};

constexpr auto
return_type(auto &&f, auto &&err)
{
  if constexpr (requires { f(std::move(get<1>(err))); }) {
    return f(std::move(get<1>(err)));
  }
  else if constexpr (std::same_as<decltype(f()), void>) {
    return;
  }
  else {
    return f();
  }
}

template<typename T>
concept is_expected_type = requires { typename T::value_type; } &&
                           requires { typename T::error_type; };

template<typename T>
constexpr auto
get_inner_return_type()
{
  if constexpr (is_expected_type<T>) {
    return typename T::value_type{};
  }
  else if constexpr (std::same_as<T, void>) {
    return;
  }
  else {
    return T{};
  }
}

template<auto NullValue>
constexpr auto
to_system_error_code(auto err, auto &&f)
{
  constexpr bool kHasReturnType = requires { f(std::move(get<1>(err))); };
  using return_type = decltype(return_type(f, err));
  using inner_return_type = decltype(get_inner_return_type<return_type>());

  return [&] -> SystemStatus<inner_return_type> {
    RPCError err_code;
    if constexpr (kHasReturnType) {
      err_code = err.error();
    }
    else {
      err_code = err;
    }

    switch (err_code) {
    case RPCError::kRPCNone:
      if constexpr (kHasReturnType) {
        return SystemStatus<inner_return_type>{f(std::move(get<1>(err)))};
      }
      else {
        static_assert(std::is_same_v<decltype(err), RPCError>);
        if constexpr (std::same_as<decltype(f()), void>) {
          return SystemStatus<void>{};
        }
        else {
          return f();
        }
      }
    case RPCError::kRPCStorageWriteFailed:
    case RPCError::kRPCNotFound:
      // can't directly combine these branches; f() isn't a valid expression in
      // the first case
      //
      // NOLINTBEGIN(bugprone-branch-clone)
      if constexpr (std::same_as<decltype(NullValue),
                                 NotFoundIsUnrecoverableError>) {
        error("Unrecoverable: Received RPCNotFound or RPCStorageWriteFailed on "
              "unexpected rpc: {}",
              err_code);
        return SystemError{SystemErrorCode::Unrecoverable};
      }
      else if constexpr (kHasReturnType) {
        return SystemStatus<return_type>{NullValue};
      }
      else if constexpr (!std::same_as<decltype(f()), void>) {
        return SystemStatus<return_type>{NullValue};
      }
      // NOLINTEND(bugprone-branch-clone)
      else {
        // This should never happen -- none of the rpc errors that have a return
        // type of void should return this kind of error code.
        assert(false);
        error("Unrecoverable: Received RPCNotFound or RPCStorageWriteFailed on "
              "unexpected rpc: {}",
              err_code);
        return SystemError{SystemErrorCode::Unrecoverable};
      }
    case RPCError::kRPCBadBlockID:
      return SystemError{SystemErrorCode::Abandon};
    default:
      error("Unrecoverable: Received unhandled error type from RPC: {}",
            err_code);
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  }();
}

} // namespace

DatabaseProxy::DatabaseProxy(DatabaseIdentifier db_identifier)
  : client_([&] -> client_t {
      if (std::holds_alternative<LocalMockDB>(db_identifier)) {
        auto mock_db = std::get<LocalMockDB>(db_identifier);
        return make_srpc_multiclient<DatabaseToExecutorInterfaceV1>(
            x23::HostPort{"localhost",
                          x23::uformat("{}", mock_db.read_port_number_)},
            false, *x23::single_bucket_map);
        // Uses just one client for local mock testing.
        // This avoids fd ulimit issues in sdk tests
      }
      if (std::holds_alternative<ShardedDB>(db_identifier)) {
        auto sharded_db = std::get<ShardedDB>(db_identifier);
        assert(!sharded_db.shards_.empty());
        if (sharded_db.shards_.size() > 1) {
          throw std::runtime_error(
              "ShardedDB with more than one shard is not yet supported");
        }
        x23::HostPort hostport;
        if (!hostport.parse(sharded_db.shards_[0].address_for_exec)) {
          throw std::runtime_error("Failed to parse database shard address");
        }
        return make_srpc_multiclient<DatabaseToExecutorInterfaceV1>(hostport,
                                                                    false);
      }
      throw std::runtime_error("unimplemented");
    }())
{}

SystemStatus<std::optional<WireStorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = client_.get_snapshot_item(block_id, addrkey);
  return to_system_error_code<std::nullopt>(
      res, [](WireStorageItem &&x) -> std::optional<WireStorageItem> {
        return std::make_optional<WireStorageItem>(std::move(x));
      });
}

SystemStatus<std::optional<Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = client_.get_contract(block_id, contract_hash);
  return to_system_error_code<std::nullopt>(res, [](Contract &&x) {
    return std::make_optional<Contract>(std::move(x));
  });
}

SystemStatus<std::optional<DeployedContractHash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = client_.get_deployment(block_id, addr);
  return to_system_error_code<std::nullopt>(res, [](DeployedContractHash &&x) {
    return std::make_optional<DeployedContractHash>(std::move(x));
  });
}

SystemStatus<std::optional<RetrocausalQueueBlock>>
DatabaseProxy::get_next_retrocausal_queue_block(BlockID block_id,
                                                AddressAndKey const &addrkey,
                                                uint64_t search_start_number)
{
  auto res = client_.get_next_retrocausal_queue_block(block_id, addrkey,
                                                      search_start_number);
  return to_system_error_code<std::nullopt>(res, [](RetrocausalQueueBlock &&x) {
    return std::make_optional<RetrocausalQueueBlock>(std::move(x));
  });
}

SystemStatus<std::optional<QueueEntry>>
DatabaseProxy::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint32_t index)
{
  auto res =
      client_.get_concurrent_retrocausal_queue_entry(block_id, addrkey, index);
  return to_system_error_code<std::nullopt>(res, [](QueueEntry &&x) {
    return std::make_optional<QueueEntry>(std::move(x));
  });
}

SystemStatus<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id,
                                  BorrowedContractBatch const &new_contracts)
{
  try {
    auto res = client_.commit_transaction(block_id, tx_id, new_contracts);
    return to_system_error_code<VoidMarker{}>(res, [] {});
  } catch (x23::XdrException const &e) {
    // Contain transport/serialization failures so they cannot escape this
    // detached executor worker and terminate hogexec. This covers both
    // XdrBadInput (client-side: oversized new contract batch) and XdrCallFailed
    // (server-side SYSTEM_ERR, e.g. HogDB's contract rehash throwing while
    // re-serializing the Contract). Both subclass XdrException.
    error("Unrecoverable: commit_transaction threw XdrException "
          "(oversized new contract batch or server-side commit failure?): {}",
          e.what());
    return SystemError{SystemErrorCode::Unrecoverable};
  } catch (std::bad_alloc const &e) {
    error("Unrecoverable: commit_transaction threw bad_alloc: {}", e.what());
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = client_.abort_transaction(block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<bool>
DatabaseProxy::try_push_new_subtransaction(BlockID block_id,
                                           TransactionID tx_id,
                                           StorageDeltaBatch const &delta_batch)
{
  if constexpr (is_debug_build()) {
    for (auto const &delta : delta_batch) {
      check_delta_type(delta.addrkey.key.key_type(), delta.delta.type());
    }
  }
  try {
    auto res =
        client_.try_push_new_subtransaction(block_id, tx_id, delta_batch);
    return to_system_error_code<false>(res, [] { return true; });
  } catch (x23::XdrException const &e) {
    // Same containment as commit_transaction: XdrBadInput (oversized delta
    // batch) and XdrCallFailed (server-side SYSTEM_ERR) both escape as
    // XdrException and must not terminate the detached executor worker.
    error("Unrecoverable: try_push_new_subtransaction threw XdrException "
          "(oversized delta batch or server-side push failure?): {}",
          e.what());
    return SystemError{SystemErrorCode::Unrecoverable};
  } catch (std::bad_alloc const &e) {
    error("Unrecoverable: try_push_new_subtransaction threw bad_alloc: {}",
          e.what());
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<void>
DatabaseProxy::pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                   uint32_t count)
{
  auto res = client_.pop_subtransactions(block_id, tx_id, count);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<DbTip>
DatabaseProxy::get_db_tip(BlockID block_id)
{
  auto res = client_.get_db_tip(block_id);
  return to_system_error_code<NotFoundIsUnrecoverableError{}>(
      res, [block_id](DbTip &&x) -> SystemStatus<DbTip> {
        if (x.id == block_id) {
          error("Unrecoverable: Received DbTip with block id equal to current "
                "block id: {} (should be parent ID)",
                x.id);
          return SystemError{SystemErrorCode::Unrecoverable};
        }
        return x;
      });
}

SystemStatus<ViewingSnapshot>
DatabaseProxy::open_viewing_snapshot()
{
  auto res = client_.open_viewing_snapshot();
  return to_system_error_code<NotFoundIsUnrecoverableError{}>(
      res,
      [](ViewingSnapshot &&x) -> SystemStatus<ViewingSnapshot> { return x; });
}

SystemStatus<void>
DatabaseProxy::close_viewing_snapshot(BlockID block_id)
{
  auto res = client_.close_viewing_snapshot(block_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

} // namespace hogd
