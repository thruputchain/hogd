#include "hogd/hogexec/runtime/database_proxy.h"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "contract.hh"
#include "coord.hh"
#include "executor_to_db_rpc.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/rpc/rpc.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

struct VoidMarker {};

template<auto NullValue>
constexpr auto
to_system_error_code(auto sys_err, auto &&f)
{
  constexpr bool kHasReturnType =
      requires { f(std::move(get<1>(sys_err.value()))); };
  auto type_lambda = [&] {
    if constexpr (kHasReturnType) {
      return f(std::move(get<1>(sys_err.value())));
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
    if (!sys_err.has_value()) [[unlikely]] {
      return SystemStatus<ret_t>{sys_err.error()};
    }

    auto err = sys_err.value();

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

DatabaseProxy::DatabaseProxy(DatabaseIdentifier db_identifier)
  : client_([&] -> RPCClient {
      if (std::holds_alternative<LocalMockDB>(db_identifier)) {
        auto mock_db = std::get<LocalMockDB>(db_identifier);
        auto res = RPCClient::dial("127.0.0.1:" +
                                   std::to_string(mock_db.read_port_number_));
        if (!res) {
          throw std::runtime_error("Failed to connect to local mock database");
        }
        return std::move(*res);
      }
      if (std::holds_alternative<ShardedDB>(db_identifier)) {
        auto sharded_db = std::get<ShardedDB>(db_identifier);
        assert(sharded_db.shards_.size() > 0);
        auto res = RPCClient::dial(sharded_db.shards_[0].address);
        if (!res) {
          throw std::runtime_error("Failed to connect to database shard");
        }
        return std::move(*res);
      }
      std::terminate();
    }())
{}

namespace {
template<uint32_t Method, StringLiteral Name>
requires is_rpc_method<DatabaseToExecutorInterfaceV1, Method, Name>
auto
client_call(RPCClient &client, auto &&...args)
{
  static_assert(Name.get() ==
                    DatabaseToExecutorInterfaceV1::proc_meta<Method>::proc_name,
                "Method name mismatch");

  using return_type =
      DatabaseToExecutorInterfaceV1::proc_meta<Method>::return_type;

  try {
    x23::Buf buf(sizeof(RPCCallHeader));
    RPCCallHeader header{.method = Method};
    std::memcpy(buf.data(), &header, sizeof(RPCCallHeader));

    (x23::serialize_to(&buf, std::forward<decltype(args)>(args)), ...);

    auto future = client.call(std::as_bytes(std::span(buf)));
    future.wait();
    assert(future.valid());
    auto res = future.get();
    if (res.size() < sizeof(RPCResponseHeader)) {
      error("RPC error: response too small: {}", res.size());
      return SystemStatus<return_type>{
          SystemError{SystemErrorCode::kUnrecoverable}};
    }
    RPCResponseHeader response_header;
    std::memcpy(&response_header, res.data(), sizeof(RPCResponseHeader));
    if (response_header.error != RPCReponseError::kRpcOk) {
      error("RPC error: {}", response_header.error);
      return SystemStatus<return_type>{
          SystemError{SystemErrorCode::kAbortTransaction}};
    }

    return_type ret;
    x23::deserialize(&ret, res.subspan(sizeof(RPCResponseHeader)));
    return SystemStatus<return_type>{std::move(ret)};
  } catch (const x23::XdrException &e) {
    error("x23 serialization error: {}", e.what());
    return SystemStatus<return_type>{
        SystemError{SystemErrorCode::kUnrecoverable}};
  }
}

} // namespace

SystemStatus<std::optional<StorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto res = client_call<100, "get_snapshot_item">(client_, block_id, addrkey);
  return to_system_error_code<std::nullopt>(res, [](StorageItem &&x) {
    return std::make_optional<StorageItem>(std::move(x));
  });
}

SystemStatus<std::shared_ptr<const Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto res = client_call<202, "get_contract">(client_, block_id, contract_hash);
  return to_system_error_code<nullptr>(res, [](const Contract &x) {
    return std::make_shared<const Contract>(x);
  });
}

SystemStatus<std::optional<SHA256Hash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto res = client_call<203, "get_deployment">(client_, block_id, addr);
  return to_system_error_code<std::nullopt>(res, [](SHA256Hash &&x) {
    return std::make_optional<SHA256Hash>(std::move(x));
  });
}

SystemStatus<std::optional<RetrocausalQueueBlock>>
DatabaseProxy::get_next_retrocausal_queue_block(BlockID block_id,
                                                AddressAndKey const &addrkey,
                                                uint64_t search_start_number)
{
  auto res =
      client_call<101, "get_next_retrocausal_queue_block">(client_, block_id,
                                                           addrkey,
                                                           search_start_number);
  return to_system_error_code<std::nullopt>(res, [](RetrocausalQueueBlock &&x) {
    return std::make_optional<RetrocausalQueueBlock>(std::move(x));
  });
}

SystemStatus<std::optional<QueueEntry>>
DatabaseProxy::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint32_t index)
{
  auto res = client_call<102, "get_concurrent_retrocausal_queue_entry">(
      client_, block_id, addrkey, index);
  return to_system_error_code<std::nullopt>(res, [](QueueEntry &&x) {
    return std::make_optional<QueueEntry>(std::move(x));
  });
}

SystemStatus<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = client_call<1, "commit_transaction">(client_, block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  auto res = client_call<2, "abort_transaction">(client_, block_id, tx_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::commit_contract_create(BlockID block_id,
                                      Contract const &contract)
{
  auto res =
      client_call<200, "commit_contract_create">(client_, block_id, contract);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<void>
DatabaseProxy::commit_contract_deploy(BlockID block_id,
                                      ContractAddress const &addr,
                                      SHA256Hash const &hash)
{
  auto res =
      client_call<201, "commit_contract_deploy">(client_, block_id, addr, hash);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

SystemStatus<bool>
DatabaseProxy::try_push_new_subtransaction(BlockID block_id,
                                           TransactionID tx_id,
                                           StorageDeltaBatch const &delta_batch)
{
  auto res = client_call<0, "try_push_new_subtransaction">(client_, block_id,
                                                           tx_id, delta_batch);
  return to_system_error_code<false>(res, [] { return true; });
}

SystemStatus<void>
DatabaseProxy::pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                   uint32_t count)
{
  auto res =
      client_call<3, "pop_subtransactions">(client_, block_id, tx_id, count);
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
