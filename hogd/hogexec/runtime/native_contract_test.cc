#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/database_proxy.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/execution_mode.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/sockio.h"

namespace hogd {

class NativeContractTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    auto contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler & /*unused*/, NativeRuntime & /*unused*/)
                 -> InvokeStatus<void> { return {}; }},
            {1,
             [](SystemCallHandler & /*unused*/,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }}});

    // Invokes another contract, returns the error code of the other contract
    auto contract_1 = std::make_unique<NativeContract>(
        std::unordered_map<MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto const &calldata = handler.get_calldata();

               if (calldata.size() !=
                   sizeof(MethodID) + sizeof(ContractAddress)) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               MethodID invoked_id =
                   *reinterpret_cast<const MethodID *>(calldata.data());
               ContractAddress invoked_addr;
               std::memcpy(invoked_addr.data(),
                           calldata.data() + sizeof(MethodID),
                           invoked_addr.size());

               auto invoke_res = handler.invoke(
                   invoked_addr, invoked_id,
                   std::numeric_limits<GasAmount>::max(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagNone));

               if (invoke_res.first) {
                 return (invoke_res.first.value() ==
                         SyscallErrorCode::kSyscallErrorCodeNone)
                            ? InvokeStatus<void>{}
                            : InvokeError{
                                  InvokeErrorCode::kInvokeDeterministic};
               }
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }}});

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract_0));
    contract_map.emplace(1, std::move(contract_1));

    auto event_port = event_server_.start_server(0);

    runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));
    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = snapshot_server_.port()});
    event_server_proxy_ = std::make_unique<EventServerProxy>(
        x23::HostPort{"localhost", event_port});
  }

  BlockSnapshotServer snapshot_server_ = BlockSnapshotServer(0);
  TestEventAggregator event_server_;

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;
  std::shared_ptr<RuntimeContext> runtime_context_;

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 StorageSnapshotCache &snapshot,
                                 BlockID block_id, TransactionID tx_id)
  {
    return SystemCallHandler(
        signed_tx, snapshot,
        PerTransactionDatabaseProxy(*db_proxy_, block_id, tx_id),
        PerTransactionEventServerProxy(*event_server_proxy_, block_id, tx_id),
        ProposalModeData{}, runtime_context_);
  }
};

TEST_F(NativeContractTest, BasicInstantiate)
{
  StorageSnapshotCache snapshot(0, *db_proxy_);
  SystemCallHandler handler = make_handler(SignedTransaction{}, snapshot, 0, 0);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  auto runtime = runtime_context_->make_new_runtime(contract, sha256(contract),
                                                    &handler, true);

  auto res = runtime->invoke(0, UINT64_MAX);

  ASSERT_TRUE(res.result_);

  res = runtime->invoke(1, UINT64_MAX);

  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::kInvokeDeterministic);
}

TEST_F(NativeContractTest, GasCostsAccumulate)
{
  NativeContractBuilder builder;
  ContractAddress address{};

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeRuntime &runtime,
         uint32_t test_amount) -> InvokeStatus<int32_t> {
        if (!runtime.consume_gas(test_amount)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }
        return 0;
      });
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      1,
      [&address](SystemCallHandler &handler,
                 NativeRuntime &runtime) -> InvokeStatus<int32_t> {
        // invoke method 0 three times, with various gas costs
        auto [r1, g1] = handler.invoke(
            address, 0, UINT64_MAX,
            Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                serialize<uint32_t>(100)
                    .value(),
            std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r1.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g1 != 100) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [r2, g2] = handler.invoke(
            address, 0, UINT64_MAX,
            Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                serialize<uint32_t>(200)
                    .value(),
            std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r2.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g2 != 200) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [r3, g3] = handler.invoke(
            address, 0, UINT64_MAX,
            Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                serialize<uint32_t>(150)
                    .value(),
            std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r3.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g3 != 150) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        if (!runtime.consume_gas(g1 + g2 + g3)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        return 0;
      });

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      2,
      [&address](SystemCallHandler &handler,
                 NativeRuntime &runtime) -> InvokeStatus<int32_t> {
        auto [r, g] =
            handler.invoke(address, 1, UINT64_MAX, Calldata{},
                           std::to_underlying(InvokeFlag::kInvokeFlagNone));
        if (!r.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (g != 100 + 200 + 150) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!runtime.consume_gas(g)) {
          return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
        }

        return 0;
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));

  runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_.setup_create_contract(contract);
  snapshot_server_.setup_create_deployment(address, sha256(contract));

  SystemCallHandler handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      {
                          .invoked_address = address,
                          .entrypoint = entrypoint_from_method_id(2),
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
              }},
      snapshot, PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
      PerTransactionEventServerProxy(*event_server_proxy_, 0, 0),
      ProposalModeData{}, runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(res.gas_consumed_, 100 + 200 + 150);
}

namespace {
auto
make_bytestring(std::string_view str)
{
  Bytestring bytestring;
  bytestring.resize_uninitialized(str.size());
  std::memcpy(bytestring.data(), str.data(), str.size());
  return bytestring;
}
} // namespace

TEST_F(NativeContractTest, LazyLoadingBytestring)
{
  // Test to verify that items are loaded lazily
  NativeContractBuilder builder;
  ContractAddress address{};
  StorageKeyContractData bad_key{};
  StorageKey bad_full_key(StorageKeyType::kStorageKeyTypeContractData);
  bad_full_key.contract_data_key() = bad_key;

  auto bstring = make_bytestring("bad");

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [&](SystemCallHandler &handler) -> InvokeStatus<void> {
        // comment this set_bytestring to verify that the test framework
        // does, in fact, throw errors if you get_bytestring from the snapshot
        auto r =
            handler.set_bytestring(bad_key, std::as_bytes(std::span(bstring)));
        if (!r.has_value() || !r.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto r2 = handler.get_bytestring(bad_key);
        if (!r2.has_value() || !r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (r2.value().value().size() != bstring.size()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (std::memcmp(r2.value().value().data(), bstring.data(),
                        bstring.size()) != 0) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));

  runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_.setup_create_contract(contract);
  snapshot_server_.setup_create_deployment(address, sha256(contract));
  snapshot_server_.setup_ban_item(
      AddressAndKey{.address = address, .key = bad_full_key});

  SystemCallHandler handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      {
                          .invoked_address = address,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
              },
      },
      snapshot, PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
      PerTransactionEventServerProxy(*event_server_proxy_, 0, 0),
      ProposalModeData{}, runtime_context_);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
}

TEST_F(NativeContractTest, LazyLoadingNNInt)
{
  // Test to verify that items are loaded lazily
  NativeContractBuilder builder;
  ContractAddress address{};
  StorageKeyContractData bad_key{};
  StorageKey bad_full_key(StorageKeyType::kStorageKeyTypeContractData);
  bad_full_key.contract_data_key() = bad_key;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [&](SystemCallHandler &handler) -> InvokeStatus<void> {
        // get_nnint() with force_load_from_snapshot=false from clean state
        // should not query db
        auto r0 = handler.get_nnint(bad_key, false);
        if (!r0.has_value() || !r0.value().has_value() ||
            r0.value().value().first.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // add ops shouldn't query db
        auto [r, _gas] =
            syscall_NonnegativeIntSetAdd_inner(handler, bad_key, nullptr, 100);
        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        auto [r2, _gas2] =
            syscall_NonnegativeIntSetAdd_inner(handler, bad_key, nullptr, -200);
        if (!r2.has_value() ||
            r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // assert that get_nnint() without a load doesn't query db
        auto r3 = handler.get_nnint(bad_key, false);
        if (!r3.has_value() || !r3.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (r3.value().value().first.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      1, [&](SystemCallHandler &handler) -> InvokeStatus<void> {
        int128_t set_value = 100;
        auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(handler, bad_key,
                                                            &set_value, 200);
        if (!r.has_value() ||
            r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // assert that get_nnint() after a set doesn't query db, even with
        // force_load_from_snapshot=true
        auto r2 = handler.get_nnint(bad_key, true);
        if (!r2.has_value() || !r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!r2.value().value().first.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // get_nnint() after a set with force_load_from_snapshot=false should
        // return the set value if it exists
        auto r3 = handler.get_nnint(bad_key, false);
        if (!r3.has_value() || !r3.value().has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        if (!r3.value().value().first.has_value()) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));

  runtime_context_ = std::make_shared<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_.setup_create_contract(contract);
  snapshot_server_.setup_create_deployment(address, sha256(contract));
  snapshot_server_.setup_ban_item(
      AddressAndKey{.address = address, .key = bad_full_key});

  {
    SystemCallHandler handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        {
                            .invoked_address = address,
                            .entrypoint = entrypoint_from_method_id(0),
                            .gas_limit = UINT64_MAX,
                            .calldata = {},
                        },
                },
        },
        snapshot, PerTransactionDatabaseProxy(*db_proxy_, 0, 0),
        PerTransactionEventServerProxy(*event_server_proxy_, 0, 0),
        ProposalModeData{}, runtime_context_);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
  }
  {
    SystemCallHandler handler = make_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        {
                            .invoked_address = address,
                            .entrypoint = entrypoint_from_method_id(1),
                            .gas_limit = UINT64_MAX,
                            .calldata = {},
                        },
                },
        },
        snapshot, 0, 1);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
  }
}

} // namespace hogd
