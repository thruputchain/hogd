#pragma once

#include <cstdint>
#include <gtest/gtest.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "account.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "genesis.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/db_interface/db_sched_server.h"
#include "hogd/hogdb/block_id_manager.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/threadpool.h"

namespace hogd {

enum class TestDB : int8_t {
  kMockInMemory = 0,
  kHogDB = 1,
};

class MockDBWrapper {
public:
  MockDBWrapper() { coord_port_ = db_sched_server_.start_server(0); }

  uint16_t exec_port() { return db_executor_server_.port(); }
  uint16_t coord_port() const { return coord_port_; }

  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> item)
  {
    in_memory_db_.write_item(addrkey, std::move(item));
  }

  void write_contract(ContractAddress const &addr,
                      std::shared_ptr<const Contract> const &contract)
  {
    in_memory_db_.write_contract(addr, contract);
  }

  void write_deployment(ContractAddress const &addr,
                        DeployedContractHash const &deployment,
                        StorageCostModel const *cost_model)
  {
    in_memory_db_.write_item(
        AddressAndKey{.address = addr,
                      .key = StorageKey(
                          StorageKeyType::kStorageKeyTypeAddressMetadata)},
        std::make_shared<const StorageItemWrapper>(deployment, cost_model));
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

  SrpcDbExecutorServer db_executor_server_ =
      SrpcDbExecutorServer(&block_manager_, 0);

  DbSchedServer db_sched_server_ = DbSchedServer(&block_manager_);
  uint16_t coord_port_ = 0;
};

class HogdbWrapper {
public:
  HogdbWrapper() : db_(open_db())
  {
    db_sched_server_.start_server(0);
    db_executor_server_.start_server(0);
  }

  uint16_t exec_port() { return db_executor_server_.port(); }
  uint16_t coord_port() { return db_sched_server_.port(); }

  void grant_storage_token(ContractAddress const &address, int128_t amount);

  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> const &item);

  void write_contract(ContractAddress const &addr,
                      std::shared_ptr<const Contract> const &contract);
  void write_deployment(ContractAddress const &addr,
                        DeployedContractHash const &deployment,
                        StorageCostModel const *cost_model);

  // different storage cost models unimplemented currently
  const StorageCostModel kDefaultCostModel = StorageCostModel(128, 1, 0, 0, 0);

  const StorageCostModel *get_default_cost_model()
  {
    return &ctx_.get_cost_model();
  }

private:
  static HogDBDAO open_db()
  {
    Directory dir = Directory::open_temporary().value();
    constexpr static size_t kDefaultHeapSize = 1024 * 1024 * 1024;
    auto db = Database::open(dir.get_path(), kDefaultHeapSize).value();
    return HogDBDAO(RAMDB(std::move(db)));
  }

  HogDBDAO db_;
  MerkleTriePrefix trie_;
  RPCContext ctx_ = RPCContext(db_, trie_);
  HogDBExecutorServer db_executor_server_ = HogDBExecutorServer(ctx_);
  HogDBSchedServer db_sched_server_ = HogDBSchedServer(ctx_);
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

        return client_config->template client<DbSched1>();
      }())
  {}

  using coord_client_t =
      DbSched1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>;

  coord_client_t &client() { return coord_client_; }

private:
  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  coord_client_t coord_client_;
};

class EventSchedWrapper {
public:
  EventSchedWrapper(uint16_t port)
    : event_sched_client_(connect_to_event_sched_server(port))
  {}

  auto &client() { return event_sched_client_; }

private:
  using client_t =
      EventSched1::client<x23::RefPtr<x23::SRpcMulticlient<x23::RpcLogger>>>;

  static client_t connect_to_event_sched_server(uint16_t port)
  {
    auto server_addr = x23::HostPort{"localhost", port}.to_addr(true);
    if (!server_addr) {
      throw std::runtime_error("Failed to connect to event server");
    }
    return x23::SRpcMulticlient<x23::RpcLogger>::make(server_addr.value())
        ->client(EventSched1{});
  }
  client_t event_sched_client_;
};

// Common setup utilities for testing native contracts.
class NativeContractTestFramework {
public:
  NativeContractTestFramework(
      bool log = false,
      BlockExecutionMode mode = BlockExecutionMode::kBlockExecutionModeProposal,
      TestDB test_db = TestDB::kMockInMemory, bool empty_genesis = true)
    : db_wrapper_(make_db_wrapper(test_db))
  {
    if (log) {
      enable_logging();
    }
    BlockIDStateManager::throw_on_error_ = true;
    initialize(mode, empty_genesis);
    HogexecMetrics::launch_log_worker();
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

  struct EphemeralTxIdentifier {
    BlockID block_id;
    TransactionID tx_id;
  };

  // Commits the current block and creates a new one.
  void advance_block(BlockExecutionMode mode =
                         BlockExecutionMode::kBlockExecutionModeProposal);

  // Discards the current block and creates a new one.
  void discard_block(BlockExecutionMode mode =
                         BlockExecutionMode::kBlockExecutionModeProposal);

  SystemCallHandler
  make_new_system_call_handler(SignedTransaction const &tx,
                               std::shared_ptr<RuntimeContext> runtime_context,
                               EphemeralTxIdentifier *identifier = nullptr)
  {
    assert(storage_snapshot_cache_);
    auto txid = allocate_txid();
    if (identifier) {
      identifier->block_id = block_number_;
      identifier->tx_id = txid;
    }
    return SystemCallHandler(tx, *storage_snapshot_cache_,
                             make_new_per_tx_db_proxy(txid),
                             make_new_per_tx_event_server_proxy(txid),
                             ProposalModeCtor{}, std::move(runtime_context));
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

  std::shared_ptr<const Contract>
  lookup_contract_from_db(SHA256Hash const &hash)
  {
    auto r = db_proxy_->get_contract(block_number_, hash);
    if (!r.has_value()) {
      throw std::runtime_error("lookup_contract_from_db: db lookup failed");
    }
    return *r;
  }

  static auto make_native_contract(int32_t contract_id)
  {
    auto contract = std::make_shared<Contract>(ContractType::kNative);
    contract->native_contract_id() = contract_id;
    return contract;
  }

  static constexpr ContractAddress make_contract_address(uint8_t id)
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
  constexpr static AddressAndKey
  make_contract_data_addrkey_matching_static_key_sdk_macro(
      ContractAddress const &address, std::string_view str)
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() =
        make_storage_key_matching_static_key_sdk_macro(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }
  constexpr static AddressAndKey
  make_contract_data_addrkey(ContractAddress const &address,
                             std::string_view str)
  {
    StorageKey key(StorageKeyType::kStorageKeyTypeContractData);
    key.contract_data_key() = make_storage_key(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }

  constexpr static AddressAndKey
  make_account_auth_data_addrkey(ContractAddress const &address)
  {
    return AddressAndKey{.address = address,
                         .key = StorageKey(
                             StorageKeyType::kStorageKeyTypeAccountAuthData)};
  }

  constexpr static AddressAndKey
  make_free_native_token_balance_addrkey(ContractAddress const &address)
  {
    return AddressAndKey{
        .address = address,
        .key =
            StorageKey(StorageKeyType::kStorageKeyTypeFreeNativeTokenBalance)};
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
  void write_contract(std::shared_ptr<const Contract> const &contract);

  void write_deployment(ContractAddress const &addr,
                        std::shared_ptr<const Contract> const &contract);

  const StorageCostModel *get_default_cost_model()
  {
    return std::visit(
        [&](auto &db_wrapper) { return db_wrapper.get_default_cost_model(); },
        db_wrapper_);
  }

  PerTransactionDatabaseProxy make_new_per_tx_db_proxy(TransactionID tx_id)
  {
    assert(db_proxy_);
    if (tx_id == UINT64_MAX) {
      tx_id = allocate_txid();
    }
    return PerTransactionDatabaseProxy(*db_proxy_, block_number_, tx_id);
  }

  PerTransactionEventServerProxy
  make_new_per_tx_event_server_proxy(TransactionID tx_id)
  {
    assert(event_server_proxy_);
    return PerTransactionEventServerProxy(*event_server_proxy_, block_number_,
                                          tx_id);
  }

  void run_genesis_initialization(
      GenesisBlock const &genesis_block,
      BlockExecutionMode mode =
          BlockExecutionMode::kBlockExecutionModeProposal);

  auto &get_coord_client() { return coord_wrapper_->client(); }

  static DeployedContractHash
  make_deployed_contract_hash(std::shared_ptr<const Contract> const &contract)
  {
    auto contract_hash = sha256(*contract);
    uint32_t contract_size = 0;

    switch (contract->contract_type()) {
    case ContractType::kNative:
    case ContractType::kMockTest:
      contract_size = 0;
      break;
    case ContractType::kWasmi:
      contract_size =
          static_cast<uint32_t>(contract->wasmi_contract().wasm.size());
      break;
    default:
      assert(false);
      contract_size = 0;
      break;
    }

    return DeployedContractHash{.hash = contract_hash,
                                .contract_size = contract_size};
  }

  std::optional<TxEventLog> get_tx_event_log(EphemeralTxIdentifier const &tx_id)
  {
    return event_server_.get_events(tx_id.block_id, tx_id.tx_id);
  }

private:
  void write_deployment(ContractAddress const &addr,
                        DeployedContractHash const &deployment,
                        StorageCostModel const *cost_model);

  void open_block(BlockExecutionMode mode)
  {
    block_number_++;
    auto r =
        coord_wrapper_->client().dbsched_create_snapshot(dbtip_block_id_,
                                                         block_number_, mode)();
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("open_block: dbsched_create_snapshot failed");
    }
    open_event_block(block_number_);
    storage_snapshot_cache_ =
        std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
  }

  void open_event_block(BlockID block_id);
  void close_event_block(BlockID block_id);

  void initialize(BlockExecutionMode mode, bool empty_genesis)
  {
    test::reset_module_cache();

    uint16_t exec_port =
        std::visit([&](auto &db_wrapper) { return db_wrapper.exec_port(); },
                   db_wrapper_);
    uint16_t coord_port =
        std::visit([&](auto &db_wrapper) { return db_wrapper.coord_port(); },
                   db_wrapper_);
    uint16_t event_server_exec_port = event_server_exec_rpc_.start_server(0);
    uint16_t event_server_sched_port = event_server_sched_rpc_.start_server(0);

    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = exec_port});
    coord_wrapper_ = std::make_unique<CoordWrapper>(coord_port);
    event_sched_wrapper_ =
        std::make_unique<EventSchedWrapper>(event_server_sched_port);

    event_server_proxy_ = std::make_unique<EventServerProxy>(
        x23::HostPort{"localhost", event_server_exec_port});

    if (!empty_genesis) {
      return;
    }

    {
      StorageCostModelXdr cost_model_xdr = {
          .ext = ExtensionPoint(0),
          .base_data_item_cost = to_xdr(int128_t{12000}),
          .bytestring_cost_per_byte = 10,
          .base_contract_deployment_cost = to_xdr(int128_t{12000}),
          .contract_deployment_cost_per_byte = to_xdr(int128_t{10}),
          .auth_per_key_cost = to_xdr(int128_t{1111}),
      };
      auto r = coord_wrapper_->client().dbsched_load_genesis_block(GenesisBlock{
          .storage_cost_model = cost_model_xdr,
      })();
      if (r != DbSchedResult::DbSchedResultOK) {
        throw std::runtime_error(
            "initialize: dbsched_load_genesis_block failed");
      }
    }

    open_block(mode);
  }

  TransactionID allocate_txid() { return next_tx_id_++; }

  TransactionID next_tx_id_ = 0;
  BlockID block_number_ = 0;
  BlockID dbtip_block_id_ = 0;

  std::unique_ptr<CoordWrapper> coord_wrapper_;
  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
  std::unique_ptr<EventSchedWrapper> event_sched_wrapper_;

  // TODO(geoff): replace with proper event server connection
  TestEventAggregator event_server_;
  TestEventAggregatorExecRpc event_server_exec_rpc_ =
      TestEventAggregatorExecRpc(event_server_);
  TestEventAggregatorSchedRpc event_server_sched_rpc_ =
      TestEventAggregatorSchedRpc(event_server_);

  std::unique_ptr<StorageSnapshotCache> storage_snapshot_cache_;

  std::variant<MockDBWrapper, HogdbWrapper> db_wrapper_;

  x23::ThreadPool pool_ = x23::ThreadPool(1);
  x23::CoroPoll cp_ = x23::CoroPoll(pool_);

  std::unique_ptr<Logger> previous_logger_ = nullptr;
};

inline std::string
test_db_to_string(const ::testing::TestParamInfo<TestDB> &info)
{
  switch (info.param) {
  case TestDB::kMockInMemory:
    return "MockInMemory";
  case TestDB::kHogDB:
    return "HogDB";
  default:
    return "unknown";
  }
};

} // namespace hogd
