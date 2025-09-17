#include "hogd/hogexec/runtime/database_proxy.h"

#include <cassert>
#include <concepts>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
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
#include "storage_delta_flags.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/rpc_common.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/vec.h"

namespace hogd {

namespace {

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
    case RPCError::kRPCStorageWriteFailed:
    case RPCError::kRPCNotFound:
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

template<auto NullValue>
constexpr auto
remove_syserr_from_call(auto syserr, auto &&f)
{
  using overall_ret_t =
      decltype(to_system_error_code(syserr.value(), std::forward(f)));
  if (!syserr.has_value()) [[unlikely]] {
    return overall_ret_t{syserr.error()};
  }

  auto err = syserr.value();
  return to_system_error_code<NullValue>(syserr, std::forward(f));
}

} // namespace

DatabaseProxy::DatabaseProxy(DatabaseIdentifier db_identifier)
  : client_([&] -> client_t {
      if (std::holds_alternative<LocalMockDB>(db_identifier)) {
        auto mock_db = std::get<LocalMockDB>(db_identifier);
        auto server_addr =
            x23::HostPort{"localhost",
                          x23::uformat("{}", mock_db.read_port_number_)}
                .to_addr(true);
        if (!server_addr) {
          throw std::runtime_error(
              std::format("failed server error: {}", server_addr.error()));
        }
        return x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
            ->client(DatabaseToExecutorInterfaceV1{});
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
        auto server_addr = hostport.to_addr(true);
        if (!server_addr) {
          throw std::runtime_error(
              std::format("failed server error: {}", server_addr.error()));
        }
        return x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
            ->client(DatabaseToExecutorInterfaceV1{});
      }
      throw std::runtime_error("unimplemented");
    }())
{}

SystemStatus<std::optional<StorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = client_.get_snapshot_item(block_id, addrkey);
  return to_system_error_code<std::nullopt>(res, [](StorageItem &&x) {
    return std::make_optional<StorageItem>(std::move(x));
  });
}

SystemStatus<std::shared_ptr<const Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = client_.get_contract(block_id, contract_hash);
  return to_system_error_code<nullptr>(res, [](const Contract &x) {
    return std::make_shared<const Contract>(x);
  });
}

SystemStatus<std::optional<SHA256Hash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = client_.get_deployment(block_id, addr);
  return to_system_error_code<std::nullopt>(res, [](SHA256Hash &&x) {
    return std::make_optional<SHA256Hash>(std::move(x));
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
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = client_.commit_transaction(block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = client_.abort_transaction(block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<bool>
DatabaseProxy::try_push_new_subtransaction(
    BlockID block_id, TransactionID tx_id, StorageDeltaBatch delta_batch,
    ContractDeployDeltaBatch contract_deploy_batch)
{
  auto res = client_.try_push_new_subtransaction(block_id, tx_id, delta_batch,
                                                 contract_deploy_batch);
  return to_system_error_code<false>(res, [] { return true; });
}

SystemStatus<void>
DatabaseProxy::pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                   uint32_t count)
{
  auto res = client_.pop_subtransactions(block_id, tx_id, count);
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
