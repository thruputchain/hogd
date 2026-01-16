#include <gtest/gtest.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "account.hh"
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
#include "storage_delta_flags.hh"
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
    snapshot_server_.start_server(/*port=*/0);

    auto event_exec_port = event_server_exec_server_.start_server(0);

    db_proxy_ = std::make_unique<DatabaseProxy>(
        LocalMockDB{.read_port_number_ = snapshot_server_.port()});
    event_server_proxy_ = std::make_unique<EventServerProxy>(
        x23::HostPort{"localhost", event_exec_port});
    event_server_.open_block(0);
  }

  BlockSnapshotServer snapshot_server_ = BlockSnapshotServer(/*block_id=*/0);
  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_server_exec_server_ =
      TestEventAggregatorExecServer(event_server_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;

  SystemCallHandler make_handler(SignedTransaction const &signed_tx,
                                 StorageSnapshotCache &snapshot,
                                 BlockID block_id, TransactionID tx_id,
                                 RuntimeContext &runtime_context)
  {
    assert(db_proxy_ != nullptr);
    assert(event_server_proxy_ != nullptr);

    return SystemCallHandler(
        signed_tx, snapshot,
        PerTransactionDatabaseProxy(*db_proxy_, block_id, tx_id),
        PerTransactionEventServerProxy(*event_server_proxy_, block_id, tx_id),
        ProposalModeCtor{}, runtime_context);
  }
};

TEST_F(NativeContractTest, BasicInstantiate)
{
  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    auto contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
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
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [](SystemCallHandler &handler,
                NativeRuntime & /*unused*/) -> InvokeStatus<void> {
               auto const &calldata = handler.get_calldata();

               if (calldata.size() !=
                   sizeof(Runtime::MethodID) + sizeof(ContractAddress)) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
               Runtime::MethodID invoked_id =
                   *reinterpret_cast<const Runtime::MethodID *>(
                       calldata.data());
               ContractAddress invoked_addr;
               std::memcpy(invoked_addr.data(),
                           calldata.data() + sizeof(Runtime::MethodID),
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
    return std::make_unique<NativeContext>(std::move(contract_map));
  };
  auto context = make_context();

  SignedTransaction signed_tx;
  signed_tx.tx.contracts_to_deploy.push_back(contract);

  SystemCallHandler handler = make_handler(signed_tx, snapshot, 0, 0, *context);

  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size = 0,
      .contract_type = ContractType::kNative,
  };

  auto runtime_res =
      context->make_new_runtime(deployed_contract_hash, &handler, true);
  ASSERT_TRUE(runtime_res.has_value());
  auto runtime = std::move(runtime_res.value());
  ASSERT_TRUE(runtime != nullptr);

  auto res = runtime->invoke(0, UINT64_MAX);

  ASSERT_TRUE(res.result_);

  res = runtime->invoke(1, UINT64_MAX);

  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::kInvokeDeterministic);
}

TEST_F(NativeContractTest, GasCostsAccumulate)
{
  NativeContractBuilder builder;
  constexpr static ContractAddress kAddress{};

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
      [](SystemCallHandler &handler,
         NativeRuntime &runtime) -> InvokeStatus<int32_t> {
        // invoke method 0 three times, with various gas costs
        auto [r1, g1] = handler.invoke(
            kAddress, 0, UINT64_MAX,
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
            kAddress, 0, UINT64_MAX,
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
            kAddress, 0, UINT64_MAX,
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
      [](SystemCallHandler &handler,
         NativeRuntime &runtime) -> InvokeStatus<int32_t> {
        auto [r, g] =
            handler.invoke(kAddress, 1, UINT64_MAX, Calldata{},
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

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      kAddress,
      DeployedContractHash{.hash = sha256(contract),
                           .contract_size = 0,
                           .contract_type = contract.contract_type()});

  SystemCallHandler handler = make_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      {
                          .invoked_address = kAddress,
                          .entrypoint = entrypoint_from_method_id(2),
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
              }},
      snapshot, 0, 0, *runtime_context);

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
  constexpr static StorageKeyContractData kBadKey{};
  StorageKey bad_full_key(StorageKeyType::kStorageKeyTypeContractData);
  bad_full_key.contract_data_key() = kBadKey;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](SystemCallHandler &handler) -> InvokeStatus<void> {
        auto bstring = make_bytestring("bad");
        // comment this set_bytestring to verify that the test framework
        // does, in fact, throw errors if you get_bytestring from the snapshot
        auto [res, _gas] = syscall_BytestringSet_inner(
            handler, kBadKey, std::as_bytes(std::span(bstring)),
            std::to_underlying(StorageItemRPCFlags::kStorageNone));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }

        // This would have failed if the bytestring was not loaded lazily:
        // we're getting the bytestring from the prior write, not the snapshot
        auto r2 = handler.get_bytestring(kBadKey);
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
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      1, [](SystemCallHandler &handler) -> InvokeStatus<void> {
        std::vector<uint8_t> bstring(100);
        RuntimeMemoryLength written_len = 0;
        auto [res, _gas] = syscall_BytestringGet_inner(handler, kBadKey,
                                                       std::as_writable_bytes(
                                                           std::span(bstring)),
                                                       written_len);
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::kInvokeDeterministic};
        }
        return {};
      });

  auto native_contract = builder.build();

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      address, DeployedContractHash{.hash = sha256(contract),
                                    .contract_size = 0,
                                    .contract_type = contract.contract_type()});
  snapshot_server_->impl()->setup_ban_item(
      AddressAndKey{.address = address, .key = bad_full_key});

  SystemCallHandler handler = make_handler(
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
      snapshot, 0, 0, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);

  SystemCallHandler handler2 = make_handler(
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
      snapshot, 0, 0, *runtime_context);
  EXPECT_ANY_THROW(handler2.root_invoke());
}

TEST_F(NativeContractTest, LazyLoadingNNInt)
{
  // Test to verify that items are loaded lazily
  ContractAddress address{};
  constexpr static StorageKeyContractData kBadKey{};

  StorageKey bad_full_key(StorageKeyType::kStorageKeyTypeContractData);
  bad_full_key.contract_data_key() = kBadKey;

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    NativeContractBuilder builder;

    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        0, [](SystemCallHandler &handler) -> InvokeStatus<void> {
          // get_nnint() with force_load_from_snapshot=false from clean state
          // should not query db
          auto r0 = handler.get_nnint(kBadKey, false);
          if (!r0.has_value() || !r0.value().has_value() ||
              r0.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          // add ops shouldn't query db
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              handler, kBadKey, nullptr, 100,
              std::to_underlying(StorageItemRPCFlags::kStorageNone));
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          auto [r2, _gas2] = syscall_NonnegativeIntSetAdd_inner(
              handler, kBadKey, nullptr, -200,
              std::to_underlying(StorageItemRPCFlags::kStorageNone));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          // assert that get_nnint() without a load doesn't query db
          auto r3 = handler.get_nnint(kBadKey, false);
          if (!r3.has_value() || !r3.value().has_value()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          if (r3.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          return {};
        });

    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        1, [](SystemCallHandler &handler) -> InvokeStatus<void> {
          int128_t set_value = 100;
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              handler, kBadKey, &set_value, 200,
              std::to_underlying(StorageItemRPCFlags::kStorageNone));
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          // assert that get_nnint() after a set doesn't query db, even with
          // force_load_from_snapshot=true
          auto r2 = handler.get_nnint(kBadKey, true);
          if (!r2.has_value() || !r2.value().has_value()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          if (!r2.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }

          // get_nnint() after a set with force_load_from_snapshot=false should
          // return the set value if it exists
          auto r3 = handler.get_nnint(kBadKey, false);
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
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() = 0;

  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      address, DeployedContractHash{.hash = sha256(contract),
                                    .contract_size = 0,
                                    .contract_type = contract.contract_type()});
  snapshot_server_->impl()->setup_ban_item(
      AddressAndKey{.address = address, .key = bad_full_key});

  {
    SystemCallHandler handler = make_handler(
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
        snapshot, 0, 0, *context);

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
        snapshot, 0, 1, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
  }
}

} // namespace hogd
