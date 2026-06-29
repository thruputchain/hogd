#include "hogd/hogexec/runtime/item/database_proxy.h"

#include <cassert>
#include <concepts>
#include <exception>
#include <format>
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

namespace {

// Runtime hash of an account address into [0, mod): FNV-1a over the 32 address
// bytes. Deterministic within a process, so an address (and therefore all of
// its items) maps to a single shard. This is a placeholder hash; a
// network-agreed function will replace it when sharding becomes externally
// visible.
size_t
shard_of_address(ContractAddress const &address, size_t mod)
{
  uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
  for (uint8_t byte : address) {
    h ^= byte;
    h *= 1099511628211ULL; // FNV-1a prime
  }
  return static_cast<size_t>(h % mod);
}

} // namespace

size_t
DatabaseProxy::look_up_shard(AddressAndKey const &key) const
{
  // Route by account address so all of an address's items live on one shard.
  return shard_of_address(key.address, clients_.size());
}

DatabaseProxy::client_t &
DatabaseProxy::client_for(AddressAndKey const & /*key*/)
{
  // Primary shard for now. To turn on per-key routing, change this one line to:
  //   return clients_[look_up_shard(key)];
  return clients_[0];
}

DatabaseProxy::DatabaseProxy(DatabaseIdentifier db_identifier)
{
  if (std::holds_alternative<LocalMockDB>(db_identifier)) {
    auto mock_db = std::get<LocalMockDB>(db_identifier);
    // A single client for local mock testing. Using one client avoids fd ulimit
    // issues in sdk tests.
    clients_.push_back(make_srpc_multiclient<DatabaseToExecutorInterfaceV1>(
        x23::HostPort{"localhost", x23::uformat("{}", mock_db.read_port_number_)},
        false, *x23::single_bucket_map));
    return;
  }
  if (std::holds_alternative<ShardedDB>(db_identifier)) {
    auto sharded_db = std::get<ShardedDB>(db_identifier);
    assert(!sharded_db.shards_.empty());
    // Connect to every discovered shard; requests are routed per key by
    // look_up_shard / client_for.
    clients_.reserve(sharded_db.shards_.size());
    for (ShardCard const &shard : sharded_db.shards_) {
      x23::HostPort hostport;
      if (!hostport.parse(shard.address_for_exec)) {
        throw std::runtime_error("Failed to parse database shard address");
      }
      clients_.push_back(
          make_srpc_multiclient<DatabaseToExecutorInterfaceV1>(hostport, false));
    }
    return;
  }
  throw std::runtime_error("unimplemented");
}

SystemStatus<std::optional<WireStorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto &client = client_for(addrkey);
  auto res = client.get_snapshot_item(block_id, addrkey);
  return to_system_error_code<std::nullopt>(
      res, [](WireStorageItem &&x) -> std::optional<WireStorageItem> {
        return std::make_optional<WireStorageItem>(std::move(x));
      });
}

SystemStatus<std::optional<Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto &client = client_for();
  auto res = client.get_contract(block_id, contract_hash);
  return to_system_error_code<std::nullopt>(res, [](Contract &&x) {
    return std::make_optional<Contract>(std::move(x));
  });
}

SystemStatus<std::optional<DeployedContractHash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto &client = client_for();
  auto res = client.get_deployment(block_id, addr);
  return to_system_error_code<std::nullopt>(res, [](DeployedContractHash &&x) {
    return std::make_optional<DeployedContractHash>(std::move(x));
  });
}

SystemStatus<std::optional<RetrocausalQueueBlock>>
DatabaseProxy::get_next_retrocausal_queue_block(BlockID block_id,
                                                AddressAndKey const &addrkey,
                                                uint64_t search_start_number)
{
  auto &client = client_for(addrkey);
  auto res = client.get_next_retrocausal_queue_block(block_id, addrkey,
                                                     search_start_number);
  return to_system_error_code<std::nullopt>(res, [](RetrocausalQueueBlock &&x) {
    return std::make_optional<RetrocausalQueueBlock>(std::move(x));
  });
}

SystemStatus<std::optional<QueueEntry>>
DatabaseProxy::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint32_t index)
{
  auto &client = client_for(addrkey);
  auto res =
      client.get_concurrent_retrocausal_queue_entry(block_id, addrkey, index);
  return to_system_error_code<std::nullopt>(res, [](QueueEntry &&x) {
    return std::make_optional<QueueEntry>(std::move(x));
  });
}

SystemStatus<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id,
                                  BorrowedContractBatch const &new_contracts)
{
  try {
    auto &client = client_for();
    auto res = client.commit_transaction(block_id, tx_id, new_contracts);
    return to_system_error_code<VoidMarker{}>(res, [] {});
  } catch (std::exception const &e) {
    // commit_transaction runs on a detached executor worker; any escaping
    // exception terminates hogexec. Contain every failure here and return a
    // structured Unrecoverable result instead. This covers:
    //   - x23::XdrBadInput   -- client-side, oversized new contract batch
    //   - x23::XdrCallFailed -- server-side SYSTEM_ERR (e.g. HogDB's contract
    //                           rehash throwing while re-serializing a
    //                           Contract)
    //   - std::bad_alloc     -- local OOM, e.g. while serializing the request
    // The diagnostic itself allocates while formatting and can throw again
    // under the same memory pressure, so guard it and fall back to the
    // no-allocation reporter before returning (cf. syscall_invoke.cc, #2719).
    try {
      error("Unrecoverable: commit_transaction failed "
            "(transport/serialization/allocation failure): {}",
            e.what());
    } catch (...) {
      report_dropped_log("commit_transaction failure diagnostic");
    }
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  auto &client = client_for();
  auto res = client.abort_transaction(block_id, tx_id);
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
    // TODO: a delta batch can span shards; split per shard once cross-shard
    // writes are handled.
    auto &client = client_for();
    auto res =
        client.try_push_new_subtransaction(block_id, tx_id, delta_batch);
    return to_system_error_code<false>(res, [] { return true; });
  } catch (std::exception const &e) {
    // Same containment as commit_transaction: this runs on a detached executor
    // worker, so contain every failure -- XdrBadInput (oversized delta batch),
    // XdrCallFailed (server-side SYSTEM_ERR), and std::bad_alloc (local OOM) --
    // and return a structured result. The diagnostic allocates and can throw
    // again under memory pressure, so guard it with the no-allocation reporter
    // before returning (cf. syscall_invoke.cc, #2719).
    try {
      error("Unrecoverable: try_push_new_subtransaction failed "
            "(transport/serialization/allocation failure): {}",
            e.what());
    } catch (...) {
      report_dropped_log("try_push_new_subtransaction failure diagnostic");
    }
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<void>
DatabaseProxy::pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                   uint32_t count)
{
  auto &client = client_for();
  auto res = client.pop_subtransactions(block_id, tx_id, count);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<DbTip>
DatabaseProxy::get_db_tip(BlockID block_id)
{
  auto &client = client_for();
  auto res = client.get_db_tip(block_id);
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
  auto &client = client_for();
  auto res = client.open_viewing_snapshot();
  return to_system_error_code<NotFoundIsUnrecoverableError{}>(
      res,
      [](ViewingSnapshot &&x) -> SystemStatus<ViewingSnapshot> { return x; });
}

SystemStatus<void>
DatabaseProxy::close_viewing_snapshot(BlockID block_id)
{
  auto &client = client_for();
  auto res = client.close_viewing_snapshot(block_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

} // namespace hogd
