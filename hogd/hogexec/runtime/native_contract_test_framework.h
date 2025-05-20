#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

// Common setup utilities for testing native contracts.
class NativeContractTestFramework {
public:
  NativeContractTestFramework(
      bool enable_logging = false,
      BlockExecutionMode mode = BlockExecutionMode::kBlockExecutionModeProposal)
  {
    if (enable_logging) {
      x23::RpcLogger::default_logfd = 1;
    }
    initialize(mode);
  }

  void advance_block(
      BlockExecutionMode mode = BlockExecutionMode::kBlockExecutionModeProposal)
  {
    auto r = block_manager_.get_block_data(block_number_);
    assert(r);
    r->close_block();

    [[maybe_unused]] auto r2 =
        block_manager_.add_child(block_number_, block_number_ + 1, mode);
    assert(r2);
    block_number_++;
    storage_snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
  }

  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);

  SystemCallHandler
  make_new_system_call_handler(SignedTransaction const &tx,
                               std::shared_ptr<RuntimeContext> runtime_context)
  {
    assert(storage_snapshot_cache_);
    return SystemCallHandler(tx, *storage_snapshot_cache_,
                             make_new_per_tx_db_proxy(), ExecutionModeData{},
                             std::move(runtime_context));
  }

  std::optional<StorageItem>
  lookup_storage_item_from_db(AddressAndKey const &addrkey)
  {
    auto r = db_proxy_->get_snapshot_item(block_number_, addrkey)();
    if (!r.has_value()) {
      throw std::runtime_error("lookup_storage_item_from_db: db lookup failed");
    }
    return *r;
  }

  static auto make_native_contract(uint32_t contract_id)
  {
    auto contract = std::make_shared<Contract>(ContractType::kNative);
    contract->native_contract_id() = contract_id;
    return contract;
  }

  static ContractAddress make_contract_address(uint8_t id)
  {
    ContractAddress address{};
    address[0] = id;
    return address;
  }
  static StorageKeyContractData make_storage_key(std::string_view str)
  {
    StorageKeyContractData key{};
    std::memcpy(key.data(), str.data(), str.size());
    return key;
  }
  static AddressAndKey
  make_contract_data_addrkey(ContractAddress const &address,
                             std::string_view str)
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() = make_storage_key(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }

  void
  setup_grant_storage_token(ContractAddress const &address, int128_t amount) {
    assert(amount >= 0);
    assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    AddressAndKey addrkey = {
      .address = address,
      .key = StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)
    };

    in_memory_db_.write_item(addrkey, std::make_shared<const StorageItemWrapper>(amount));
  }

private:
  void initialize(BlockExecutionMode mode)
  {
    constexpr static uint32_t kPortNumber = 10000;
    db_executor_server_.start_server(kPortNumber);

    db_proxy_ =
        std::make_unique<DatabaseProxy>(cp_, LocalMockDB{.read_port_number_ =
                                                             10000});
    [[maybe_unused]] auto res = block_manager_.add_child(0, 1, mode);
    assert(res);
    block_number_++;
    storage_snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
  }

  TransactionID allocate_txid() { return next_tx_id_++; }
  PerTransactionDatabaseProxy make_new_per_tx_db_proxy()
  {
    assert(db_proxy_);
    return PerTransactionDatabaseProxy(*db_proxy_, block_number_,
                                       allocate_txid());
  }

  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  TransactionID next_tx_id_ = 0;
  std::unique_ptr<DatabaseProxy> db_proxy_;
  BlockID block_number_ = 0;
  std::unique_ptr<StorageSnapshotCache> storage_snapshot_cache_;

  DbExecutorServer db_executor_server_ = DbExecutorServer(&block_manager_);
};

} // namespace hogd
