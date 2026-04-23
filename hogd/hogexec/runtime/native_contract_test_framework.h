#pragma once

#include <gtest/gtest.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "contract.hh"
#include "db_sched.hh"
#include "event.hh"
#include "event_rpc.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/db_interface/block_manager.h"
#include "hogd/db_interface/db_executor_server.h"
#include "hogd/db_interface/db_sched_server.h"
#include "hogd/hogdb/block_id_manager.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/mockdb/in_memory_database.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "hogd/version.h"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "sched_event.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/format.h"
#include "x23/refcnt.h"
#include "x23/rpc.h"
#include "x23/sockio.h"
#include "x23/srpc.h"
#include "x23/x23.h"

namespace hogd {

// GTest custom printer for XDR enums - prints enum names instead of numbers
// Must be in hogd namespace for ADL to find it
template<x23::is_xdr_enum T>
void
PrintTo(const T &value, ::std::ostream *os) // NOLINT
{
  *os << x23::show(value);
}

enum class TestDB : int8_t {
  kMockInMemory = 0,
  kHogDB = 1,
};

class MockDBWrapper {
public:
  MockDBWrapper()
  {
    db_sched_server_.start_server(/*port=*/0);
    db_executor_server_.start_server(/*port=*/0);
  }

  uint16_t exec_port() { return db_executor_server_.port(); }
  uint16_t coord_port() { return db_sched_server_.port(); }

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
                      .key = StorageKey(StorageKeyType::Metadata)},
        std::make_shared<const StorageItemWrapper>(deployment, cost_model));
  }

  void grant_storage_token(ContractAddress const &address, int128_t amount)
  {
    assert(amount >= 0);
    assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

    AddressAndKey addrkey = {.address = address,
                             .key = StorageKey(
                                 StorageKeyType::FreeNativeTokenBalance)};

    write_item(addrkey, std::make_shared<const StorageItemWrapper>(
                            amount, get_default_cost_model()));
  }

  static std::map<ContractAddress, int128_t>
  get_hogs_transferred(BlockHeight /*unused*/)
  {
    // unimplemented
    error("get_hogs_transferred in MockDB: unimplemented");
    return std::map<ContractAddress, int128_t>();
  }

  const StorageCostModel *get_default_cost_model() const
  {
    return block_manager_.get_cost_model();
  }

private:
  InMemoryDatabase in_memory_db_ = InMemoryDatabase();
  BlockManager block_manager_ = BlockManager(0, &in_memory_db_);

  DbExecutorServer db_executor_server_ = DbExecutorServer(&block_manager_);

  DbSchedServer db_sched_server_ = DbSchedServer(&block_manager_);
};

class CoordWrapper {
public:
  CoordWrapper(uint16_t port)
    : coord_client_([port] {
        return make_srpc_client<DbSched1>(x23::HostPort{"localhost", port},
                                          false);
      }())
  {}

  using coord_client_t =
      DbSched1::client<x23::SRpcStream<x23::RpcLogger>::ClientRef>;

  coord_client_t &client() { return coord_client_; }

private:
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

class EventServerWrapper {
public:
  EventServerWrapper()
  {
    event_server_exec_server_.start_server(0);
    event_server_sched_server_.start_server(0);
  }

  uint16_t exec_port() { return event_server_exec_server_.port(); }
  uint16_t sched_port() { return event_server_sched_server_.port(); }

  EventCard make_event_card(std::string const &addr = "localhost")
  {
    return EventCard{.protocol = version::kHogdProtocol(),
                     .upid = {},
                     .address_for_exec =
                         std::format("{}:{}", addr, exec_port()).c_str(),
                     .address_for_sched =
                         std::format("{}:{}", addr, sched_port()).c_str()};
  }

  x23::Async<std::optional<TxEventLog>> get_events(BlockID block_id,
                                                   TransactionID tx_id)
  {
    return event_server_.get_events(block_id, tx_id);
  }

  x23::Async<std::optional<TxEventLog>>
  get_events_by_hash(BlockID block_id, SHA256Hash const &tx_hash)
  {
    return event_server_.get_events_by_hash(block_id, tx_hash);
  }

  // Blocking control for testing delays
  void block_emit_events() { event_server_.block_emit_events(); }
  void unblock_emit_events() { event_server_.unblock_emit_events(); }

private:
  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_server_exec_server_ =
      TestEventAggregatorExecServer(event_server_);
  TestEventAggregatorSchedServer event_server_sched_server_ =
      TestEventAggregatorSchedServer(event_server_);
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
    if (log || do_logging_env()) {
      enable_logging();
    }
    BlockIDStateManager::throw_on_error_ = true;
    initialize(mode, empty_genesis);
    HogexecMetrics::launch_log_worker();
  }

  static bool do_logging_env() noexcept
  {
    if (char *p = getenv("HOGEXEC_TRACE")) {
      if (int n = atoi(p); n > 0) {
        return true;
      }
    }
    return false;
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
  void advance_block(
      BlockExecutionMode mode = BlockExecutionMode::kBlockExecutionModeProposal,
      DbSchedResult expected_dbsched_commit_snapshot_result =
          DbSchedResult::DbSchedResultOK);

  // Closes the current block expecting prepare_committed_changes to fail,
  // then abandons (never commits). Asserts the DAO db hash is unchanged —
  // proving that block rejection before any DAO mutation is safe.
  void reject_block(BlockExecutionMode mode =
                        BlockExecutionMode::kBlockExecutionModeProposal);

  // Discards the current block and creates a new one.
  void discard_block(BlockExecutionMode mode =
                         BlockExecutionMode::kBlockExecutionModeProposal);

  [[deprecated("use the Transaction version instead")]] SystemCallHandler
  make_new_system_call_handler(SignedTransaction const &tx,
                               RuntimeContext &runtime_context,
                               EphemeralTxIdentifier *identifier = nullptr)
  {
    return make_new_system_call_handler(tx.tx, runtime_context, identifier);
  }
  SystemCallHandler
  make_new_system_call_handler(Transaction const &tx,
                               RuntimeContext &runtime_context,
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
                             ProposalModeCtor{}, runtime_context);
  }

  SystemCallHandler make_new_system_call_handler_validation(
      Transaction const &tx, RuntimeContext &runtime_context,
      NondeterministicProposalData proposal_data)
  {
    auto txid = allocate_txid();
    return SystemCallHandler(tx, *storage_snapshot_cache_,
                             make_new_per_tx_db_proxy(txid),
                             make_new_per_tx_event_server_proxy(txid),
                             ValidationModeCtor{std::move(proposal_data)},
                             runtime_context);
  }

  // keeps a reference to the storage snapshot cache to avoid invalidating it
  // when the block is advanced
  std::shared_ptr<StorageSnapshotCache> get_storage_snapshot_cache() const
  {
    return storage_snapshot_cache_;
  }

  std::optional<WireStorageItem>
  lookup_storage_item_from_db(AddressAndKey const &addrkey)
  {
    auto r = db_proxy_->get_snapshot_item(block_number_, addrkey);
    if (!r.has_value()) {
      throw std::runtime_error("lookup_storage_item_from_db: db lookup failed");
    }
    return *r;
  }

  std::optional<const Contract> lookup_contract_from_db(SHA256Hash const &hash)
  {
    auto r = db_proxy_->get_contract(block_number_, hash);
    if (!r.has_value()) {
      throw std::runtime_error("lookup_contract_from_db: db lookup failed");
    }
    return std::move(*r);
  }

  std::optional<DeployedContractHash>
  lookup_deployment_from_db(ContractAddress const &addr)
  {
    auto r = db_proxy_->get_deployment(block_number_, addr);
    if (!r.has_value()) {
      throw std::runtime_error("lookup_deployment_from_db: db lookup failed");
    }
    return *r;
  }

  static auto make_native_contract(int32_t contract_id)
  {
    auto contract = std::make_shared<Contract>(ContractType::kNative);
    contract->native_contract_id() =
        static_cast<HogexecPrecompiledNativeContracts>(contract_id);
    return contract;
  }

  static constexpr ContractAddress make_contract_address(uint8_t id)
  {
    ContractAddress address{};
    address[0] = id;
    return address;
  }
  constexpr static StorageKeyContractData make_storage_key(std::string_view str)
  {
    StorageKeyContractData key{};
    for (size_t i = 0; i < str.size(); i++) {
      key[i] = str[i];
    }
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
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() =
        make_storage_key_matching_static_key_sdk_macro(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }
  constexpr static AddressAndKey
  make_contract_data_addrkey(ContractAddress const &address,
                             std::string_view str)
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = make_storage_key(str);
    return AddressAndKey{.address = address, .key = std::move(key)};
  }

  constexpr static AddressAndKey
  make_address_auth_data_addrkey(ContractAddress const &address)
  {
    return AddressAndKey{.address = address,
                         .key = StorageKey(StorageKeyType::AuthData)};
  }

  constexpr static AddressAndKey
  make_replay_cache_addrkey(ContractAddress const &address)
  {
    return AddressAndKey{.address = address,
                         .key = StorageKey(StorageKeyType::ReplayCache)};
  }

  constexpr static AddressAndKey
  make_free_native_token_balance_addrkey(ContractAddress const &address)
  {
    return AddressAndKey{.address = address,
                         .key = StorageKey(
                             StorageKeyType::FreeNativeTokenBalance)};
  }

  static Bytestring make_bytestring(std::string_view str)
  {
    Bytestring bytestring;
    bytestring.resize_uninitialized(str.size());
    std::memcpy(bytestring.data(), str.data(), str.size());
    return bytestring;
  }

  static auto make_bytestring_delta(std::string_view str)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeBytestringSet);
    delta.bytestring() = make_bytestring(str);
    return delta;
  }

  static auto make_delete_delta()
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeDelete);
    return delta;
  }

  static auto make_nnint_set_add_delta(int128_t set, int128_t add)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeNonnegativeIntSetAdd);
    delta.nnint().set_value = to_xdr(set);
    delta.nnint().add_value = to_xdr(add);
    return delta;
  }

  static auto make_nnint_add_delta(int128_t add)
  {
    StorageDelta delta(StorageDeltaType::kDeltaTypeNonnegativeIntAdd);
    delta.nnint_add() = to_xdr(add);
    return delta;
  }

  void setup_grant_storage_token(ContractAddress const &address,
                                 int128_t amount);
  void write_item(AddressAndKey const &addrkey,
                  std::shared_ptr<const StorageItemWrapper> item);
  void write_contract(std::shared_ptr<const Contract> const &contract);

  void write_deployment(ContractAddress const &addr,
                        std::shared_ptr<const Contract> const &contract);

  static const StorageCostModel *get_default_cost_model()
  {
    static StorageCostModel default_cost_model =
        StorageCostModel::default_cost_model();
    return &default_cost_model;
  }

  PerTransactionDatabaseProxy make_new_per_tx_db_proxy(TransactionID tx_id)
  {
    assert(db_proxy_);
    if (tx_id == std::numeric_limits<TransactionID>::max()) {
      tx_id = allocate_txid();
    }
    return PerTransactionDatabaseProxy(*db_proxy_, block_number_, tx_id);
  }

  // Only for use in tests if precise control over rpcs is needed
  DatabaseProxy &get_db_proxy() { return *db_proxy_; }
  BlockID get_block_number() const { return block_number_; }

  DbTip get_db_tip() const
  {
    auto r = db_proxy_->get_db_tip(block_number_);
    if (!r) {
      throw std::runtime_error("get_db_tip: db_proxy_->get_db_tip failed");
    }
    return *r;
  }

  PerTransactionEventServerProxy
  make_new_per_tx_event_server_proxy(TransactionID tx_id)
  {
    assert(event_server_proxy_);
    return PerTransactionEventServerProxy(*event_server_proxy_, block_number_,
                                          tx_id);
  }

  void run_genesis_initialization(
      GenesisConfiguration const &genesis_configuration,
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
                                .contract_size = contract_size,
                                .contract_type = contract->contract_type()};
  }

  x23::Async<std::optional<TxEventLog>>
  get_tx_event_log(EphemeralTxIdentifier const &tx_id)
  {
    return event_server_wrapper_.get_events(tx_id.block_id, tx_id.tx_id);
  }

  std::map<ContractAddress, int128_t> get_hogs_transferred(BlockHeight height)
  {
    return std::visit(
        [&](auto &db_wrapper) {
          return db_wrapper.get_hogs_transferred(height);
        },
        db_wrapper_);
  }

  // Only valid when test_db == kHogDB. Returns the underlying HogDB DAO
  // for direct storage inspection in tests.
  HogDBDAO &get_hogdb_dao()
  {
    return std::get<HogdbWrapper>(db_wrapper_).get_db();
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
                                                         block_number_, mode);
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
    uint16_t exec_port =
        std::visit([&](auto &db_wrapper) { return db_wrapper.exec_port(); },
                   db_wrapper_);
    uint16_t coord_port =
        std::visit([&](auto &db_wrapper) { return db_wrapper.coord_port(); },
                   db_wrapper_);

    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = exec_port});
    coord_wrapper_ = std::make_unique<CoordWrapper>(coord_port);
    event_sched_wrapper_ =
        std::make_unique<EventSchedWrapper>(event_server_wrapper_.sched_port());

    event_server_proxy_ = std::make_unique<EventServerProxy>(
        x23::HostPort{"localhost", event_server_wrapper_.exec_port()});

    if (!empty_genesis) {
      return;
    }

    {
      GenesisConfiguration genesis_configuration{
          .storage_cost_model = get_default_cost_model()->to_xdr(),
      };
      uint64_t network_id = 0x0;
      SHA256Hash txset_hash{};
      SHA256Hash events_hash{};
      auto r = coord_wrapper_->client().dbsched_load_genesis_configuration(
          genesis_configuration, network_id, txset_hash, events_hash);
      if (r != DbSchedResult::DbSchedResultOK) {
        throw std::runtime_error(
            "initialize: dbsched_load_genesis_configuration failed");
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
  EventServerWrapper event_server_wrapper_;

  std::shared_ptr<StorageSnapshotCache> storage_snapshot_cache_;

  std::variant<MockDBWrapper, HogdbWrapper> db_wrapper_;
  std::unique_ptr<Logger> previous_logger_ = nullptr;
};

inline std::string
test_db_to_string(const ::testing::TestParamInfo<TestDB> info)
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
