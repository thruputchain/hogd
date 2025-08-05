#pragma once

#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "contract.hh"
#include "db_coord.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_coord_server.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_coord.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/rpc.h"
#include "x23/threadpool.h"

namespace hogd {

enum class TestDB : int8_t {
  kMockInMemory = 0,
  kHogDB = 1,
};

class MockDBWrapper {
public:
  MockDBWrapper() { db_coord_server_.start_server(0); }

  uint16_t exec_port() const { return db_executor_server_.port(); }
  uint16_t coord_port() { return db_coord_server_.port(); }

  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> item)
  {
    in_memory_db_.write_item(addrkey, std::move(item));
  }

  void write_contract(ContractAddress const &addr,
                      std::shared_ptr<const Contract> contract)
  {
    in_memory_db_.write_contract(addr, std::move(contract));
  }

  void write_deployment(ContractAddress const &addr,
                        SHA256Hash const &deployment)
  {
    in_memory_db_.write_deployment(addr, deployment);
  }

  void grant_storage_token(ContractAddress const &address, int128_t amount)
  {
    assert(amount >= 0);
    assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    AddressAndKey addrkey = {
        .address = address,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};

    write_item(addrkey, std::make_shared<const StorageItemWrapper>(
                            amount, get_default_cost_model()));
  }

  const StorageCostModel *get_default_cost_model() const
  {
    return &block_manager_.cost_model_;
  }

private:
  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);

  RPCDbExecutorServer db_executor_server_ =
      RPCDbExecutorServer(&block_manager_, 0);

  DbCoordServer db_coord_server_ = DbCoordServer(&block_manager_);
};

class HogdbWrapper {
public:
  HogdbWrapper() : db_(open_db())
  {
    db_coord_server_.start_server(0);
    auto r = db_executor_server_.start_server(0);
    if (!r) {
      throw std::runtime_error("HogdbWrapper: start_server failed");
    }
    std::println("finished starting servers");
  }

  uint16_t exec_port() const { return db_executor_server_.port(); }
  uint16_t coord_port() { return db_coord_server_.port(); }

  void grant_storage_token(ContractAddress const &address, int128_t amount);

  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> const &item);

  void write_contract(ContractAddress const &addr,
                      std::shared_ptr<const Contract> const &contract);
  void write_deployment(ContractAddress const &addr,
                        SHA256Hash const &deployment);

  // different storage cost models unimplemented currently
  const StorageCostModel kDefaultCostModel = StorageCostModel(0, 0, 0, 0, 0);

  const StorageCostModel *get_default_cost_model() const
  {
    return &kDefaultCostModel;
  }

private:
  static ItemKey make_item_key(const AddressAndKey &addr)
  {
    if (addr.key.key_type() == StorageKeyType::kStorageKeyTypeContractData)
      return ItemKey(addr.address, {}, true);
    return ItemKey(addr.address, addr.key.contract_data_key());
  }

  static HogDatabase open_db()
  {
    Directory dir = Directory::open_temporary().value();
    constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
    auto db = Database::open(dir.get_path(), kDefaultHeapSize).value();
    return HogDatabase(std::move(db));
  }

  HogDatabase db_;
  RPCContext ctx_ = RPCContext(db_);
  HogDBExecutorServer db_executor_server_ = HogDBExecutorServer(ctx_);
  HogDBCoordServer db_coord_server_ = HogDBCoordServer(ctx_);
};

class CoordWrapper {
public:
  CoordWrapper(uint16_t port)
    : coord_client_([this, port] {
        auto fd = cp_.tcpconnect("localhost", std::to_string(port))();
        if (!fd) {
          throw std::runtime_error("Failed to connect to coord server");
        }
        auto client_config = x23::rpc_stream_logged(cp_, *fd);

        return client_config.template client<DbCoord1>();
      }())
  {}

  using coord_client_t =
      DbCoord1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  coord_client_t &client() { return coord_client_; }

private:
  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  coord_client_t coord_client_;
};

// Common setup utilities for testing native contracts.
class NativeContractTestFramework {
public:
  NativeContractTestFramework(
      bool log = false,
      BlockExecutionMode mode = BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB test_db = TestDB::kMockInMemory)
    : db_wrapper_(make_db_wrapper(test_db))
  {
    if (log) {
      enable_logging();
    }
    initialize(mode);
  }

  void enable_logging();

  static std::variant<MockDBWrapper, HogdbWrapper>
  make_db_wrapper(TestDB test_db)
  {
    switch (test_db) {
    case TestDB::kMockInMemory:
      return std::variant<MockDBWrapper, HogdbWrapper>(
          std::in_place_type<MockDBWrapper>);
    case TestDB::kHogDB:
      return std::variant<MockDBWrapper, HogdbWrapper>(
          std::in_place_type<HogdbWrapper>);
    default:
      throw std::runtime_error("make_db_wrapper: invalid test_db");
    }
  }

  ~NativeContractTestFramework()
  {
    if (previous_logger_) {
      swap_logger(std::move(previous_logger_));
    }
  }

  void advance_block(BlockExecutionMode mode =
                         BlockExecutionMode::kBlockExecutionModeProposal);

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
    auto r = db_proxy_->get_snapshot_item(block_number_, addrkey);
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
  static StorageKeyContractData
  make_storage_key_matching_static_key_sdk_macro(std::string_view str)
  {
    // the sdk puts quotes around the string input to the static_key macro
    std::string sdk_input = "\"" + std::string(str) + "\"";
    return StorageKeyContractData{
        // include the null terminator
        sha256(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(
                                            sdk_input.data()),
                                        sdk_input.size()))};
  }
  static AddressAndKey make_contract_data_addrkey_matching_static_key_sdk_macro(
      ContractAddress const &address, std::string_view str)
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() =
        make_storage_key_matching_static_key_sdk_macro(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }
  static AddressAndKey
  make_contract_data_addrkey(ContractAddress const &address,
                             std::string_view str)
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() = make_storage_key(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }

  static Bytestring make_bytestring(std::string_view str)
  {
    Bytestring bytestring;
    bytestring.resize_uninitialized(str.size());
    std::memcpy(bytestring.data(), str.data(), str.size());
    return bytestring;
  }

  void setup_grant_storage_token(ContractAddress const &address,
                                 int128_t amount);
  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> item);
  void write_contract(std::shared_ptr<const Contract> contract);
  void write_deployment(ContractAddress const &addr,
                        SHA256Hash const &deployment);

  const StorageCostModel *get_default_cost_model() const
  {
    return std::visit(
        [&](auto &db_wrapper) { return db_wrapper.get_default_cost_model(); },
        db_wrapper_);
  }

private:
  void initialize(BlockExecutionMode mode)
  {
    uint16_t exec_port =
        std::visit([&](auto &db_wrapper) { return db_wrapper.exec_port(); },
                   db_wrapper_);
    uint16_t coord_port =
        std::visit([&](auto &db_wrapper) { return db_wrapper.coord_port(); },
                   db_wrapper_);

    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = exec_port});
    coord_wrapper_ = std::make_unique<CoordWrapper>(coord_port);

    auto r = coord_wrapper_->client().dbcoord_create_snapshot(0, 1, mode)();
    if (r != DbResult::DbResultOK) {
      throw std::runtime_error("initialize: dbcoord_create_snapshot failed");
    }

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

  TransactionID next_tx_id_ = 0;
  BlockID block_number_ = 0;

  std::unique_ptr<CoordWrapper> coord_wrapper_;
  std::unique_ptr<DatabaseProxy> db_proxy_;

  std::unique_ptr<StorageSnapshotCache> storage_snapshot_cache_;

  std::variant<MockDBWrapper, HogdbWrapper> db_wrapper_;

  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::unique_ptr<Logger> previous_logger_ = nullptr;
};

} // namespace hogd
