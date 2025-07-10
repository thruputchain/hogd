#include "hogd/hogexec/runtime/database_proxy.h"

#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "contract.hh"
#include "coord.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"

namespace hogd {

namespace {

std::optional<DatabaseToExecutorInterfaceV1::client<
    x23::SRpcStream<x23::RpcLogger>::ClientRef>>
connect_to_local_mock_database(x23::CoroPoll &cp, LocalMockDB local_db)
{
  auto fd =
      cp.tcpconnect("localhost", std::to_string(local_db.read_port_number_))();
  if (!fd) {
    return std::nullopt;
  }
  auto client_config = x23::srpc_stream_logged(*fd);
  return client_config->template client<DatabaseToExecutorInterfaceV1>();
}

std::optional<DatabaseToExecutorInterfaceV1::client<
    x23::SRpcStream<x23::RpcLogger>::ClientRef>>
connect_to_db_shard(x23::CoroPoll &cp, ShardCard const &shard)
{
  x23::HostPort host_addr;
  if (!host_addr.parse(std::string(shard.address))) {
    throw std::runtime_error("Failed to parse shard address");
  }
  auto addr = host_addr.to_addr();
  if (!addr) {
    throw std::runtime_error("Failed to parse shard address");
  }
  auto fd = cp.tcpconnect(&addr.value())();
  if (!fd) {
    return std::nullopt;
  }
  auto client_config = x23::srpc_stream_logged(*fd);
  return client_config->template client<DatabaseToExecutorInterfaceV1>();
}
struct VoidMarker {};

template<auto NullValue>
constexpr auto
to_system_error_code(auto err, auto &&f)
{
  constexpr bool kHasReturnType = requires { f(std::move(get<1>(err))); };
  auto type_lambda = [&] {
    if constexpr (kHasReturnType) {
      return f(std::move(get<1>(err)));
    }
    else if constexpr (std::same_as<decltype(f()), void>) {
      return;
    }
    else {
      return f();
    }
  };

  using ret_t = decltype(type_lambda());

  return [&] -> SystemStatus<ret_t> {
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
        return f(std::move(get<1>(err)));
      }
      else {
        static_assert(std::is_same_v<decltype(err), RPCError>);
        if constexpr (std::same_as<decltype(f()), void>) {
          return {};
        }
        else {
          return SystemStatus<ret_t>{f()};
        }
      }
    case RPCError::kRPCNoStorageItem:
    case RPCError::kRPCNoContractDeployed:
    case RPCError::kRPCNoQueueEntry:
    case RPCError::kRPCStorageWriteFailed:
      // can't directly combine these branches; f() isn't a valid expression in
      // the first case NOLINTBEGIN(bugprone-branch-clone)
      if constexpr (kHasReturnType) {
        return NullValue;
      }
      else if constexpr (!std::same_as<decltype(f()), void>) {
        return NullValue;
      }
      // NOLINTEND(bugprone-branch-clone)
      else {
        // This should never happen -- none of the rpc errors that have a return
        // type of void should return this kind of error code.
        assert(false);
        return SystemError{SystemErrorCode::kUnrecoverable};
      }
    case RPCError::kRPCBadBlockID:
      return SystemError{SystemErrorCode::kAbortTransaction};
    default:
      return SystemError{SystemErrorCode::kUnrecoverable};
    }
  }();
}

} // namespace

DatabaseProxy::DatabaseProxy(x23::CoroPoll &cp,
                             DatabaseIdentifier db_identifier)
  : cp_(cp), read_client_([&] {
      if (std::holds_alternative<LocalMockDB>(db_identifier)) {
        auto mock_db = std::get<LocalMockDB>(db_identifier);

        auto out = connect_to_local_mock_database(cp, mock_db);

        while (!out) {
          perror(std::format("connect to local mock db read server localhost "
                             "port {} failed, "
                             "retrying...",
                             mock_db.read_port_number_)
                     .c_str());
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          out = connect_to_local_mock_database(cp, mock_db);
        }
        return *out;
      }
      if (std::holds_alternative<ShardedDB>(db_identifier)) {
        auto sharded_db = std::get<ShardedDB>(db_identifier);
        if (sharded_db.shards_.size() == 1) {
          auto out = connect_to_db_shard(cp, sharded_db.shards_[0]);
          while (!out) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            out = connect_to_db_shard(cp, sharded_db.shards_[0]);
          }
          return *out;
        }
        // unimplemented shard count
        std::terminate(); // TODO(geoff): unimplemented
      }
      // unknown database specification type
      std::terminate();
    }())
{}

SystemStatus<std::optional<StorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = read_client_.get_snapshot_item(block_id, addrkey);
  return to_system_error_code<std::nullopt>(res, [](StorageItem &&x) {
    return std::make_optional<StorageItem>(std::move(x));
  });
}

SystemStatus<std::shared_ptr<const Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = read_client_.get_contract(block_id, contract_hash);
  return to_system_error_code<nullptr>(res, [](const Contract &x) {
    return std::make_shared<const Contract>(x);
  });
}

SystemStatus<std::optional<SHA256Hash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = read_client_.get_deployment(block_id, addr);
  return to_system_error_code<std::nullopt>(res, [](SHA256Hash &&x) {
    return std::make_optional<SHA256Hash>(std::move(x));
  });
}

SystemStatus<std::optional<RetrocausalQueueBlock>>
DatabaseProxy::get_next_retrocausal_queue_block(BlockID block_id,
                                                AddressAndKey const &addrkey,
                                                uint64_t search_start_number)
{
  auto res = read_client_.get_next_retrocausal_queue_block(block_id, addrkey,
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
      read_client_.get_concurrent_retrocausal_queue_entry(block_id, addrkey,
                                                          index);
  return to_system_error_code<std::nullopt>(res, [](QueueEntry &&x) {
    return std::make_optional<QueueEntry>(std::move(x));
  });
}

SystemStatus<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = read_client_.commit_transaction(block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = read_client_.abort_transaction(block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::commit_contract_create(BlockID block_id,
                                      Contract const &contract)
{
  auto res = read_client_.commit_contract_create(block_id, contract);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::commit_contract_deploy(BlockID block_id,
                                      ContractAddress const &addr,
                                      SHA256Hash const &hash)
{
  auto res = read_client_.commit_contract_deploy(block_id, addr, hash);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<bool>
DatabaseProxy::try_push_new_subtransaction(BlockID block_id,
                                           TransactionID tx_id,
                                           StorageDeltaBatch const &delta_batch)
{
  auto ts = std::chrono::steady_clock::now();
  auto res =
      read_client_.try_push_new_subtransaction(block_id, tx_id, delta_batch);
  auto te = std::chrono::steady_clock::now();
  std::println("try_push_new_subtransaction took {}us",
               std::chrono::duration_cast<std::chrono::microseconds>(te - ts)
                   .count());
  return to_system_error_code<false>(res, [] { return true; });
}

SystemStatus<void>
DatabaseProxy::pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                   uint32_t count)
{
  auto res = read_client_.pop_subtransactions(block_id, tx_id, count);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

StorageDeltaBatch
StorageDeltaBatchBuilder::build()
{
  StorageDeltaBatch out;
  out.append_range(positive_storage_deltas_);
  out.append_range(storage_deltas_);
  out.append_range(negative_storage_deltas_);
  return out;
}

void
StorageDeltaBatchBuilder::add_delta(AddressAndKey const &addrkey,
                                    StorageDelta const &delta,
                                    StorageItemRPCFlagSet flags)
{
  if (addrkey.key.key_type() ==
      StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance) {
    assert(delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd ||
           delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd);

    int128_t add_value = [&] {
      if (delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd) {
        return from_xdr(delta.nnint().add_value);
      }
      assert(delta.type() == StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
      return from_xdr(delta.nnint_add());
    }();

    if (add_value > 0) {
      positive_storage_deltas_.push_back(FlaggedStorageDelta{.addrkey = addrkey,
                                                             .delta = delta,
                                                             .flags = flags});
    }
    else {
      negative_storage_deltas_.push_back(FlaggedStorageDelta{.addrkey = addrkey,
                                                             .delta = delta,
                                                             .flags = flags});
    }
  }
  else {
    // TODO(geoff): prioritize deletions
    storage_deltas_.push_back(FlaggedStorageDelta{.addrkey = addrkey,
                                                  .delta = delta,
                                                  .flags = flags});
  }
}

} // namespace hogd
