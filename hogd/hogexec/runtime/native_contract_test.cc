#include <gtest/gtest.h>

#include <array>
#include <cassert>
#include <cstddef>
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

#include "built_contracts/initial_token_claim.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/hogexec/runtime/test_util/block_snapshot_server.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/sockio.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
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
    event_server_.open_block(0)();
  }

  BlockSnapshotServer snapshot_server_ = BlockSnapshotServer(/*block_id=*/0);
  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_server_exec_server_ =
      TestEventAggregatorExecServer(event_server_);

  std::unique_ptr<DatabaseProxy> db_proxy_;
  std::unique_ptr<EventServerProxy> event_server_proxy_;

  SystemCallHandler make_handler(Transaction const &tx,
                                 StorageSnapshotCache &snapshot,
                                 BlockID block_id, TransactionID tx_id,
                                 RuntimeContext &runtime_context)
  {
    assert(db_proxy_ != nullptr);
    assert(event_server_proxy_ != nullptr);

    return SystemCallHandler(
        tx, snapshot, PerTransactionDatabaseProxy(*db_proxy_, block_id, tx_id),
        PerTransactionEventServerProxy(*event_server_proxy_, block_id, tx_id),
        ProposalModeCtor{}, runtime_context);
  }
};

TEST_F(NativeContractTest, BasicInstantiate)
{
  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
    fns_0.emplace(0,
                  [](NativeSyscallContext & /*unused*/) noexcept
                      -> InvokeStatus<void> { return {}; });
    fns_0.emplace(1,
                  [](NativeSyscallContext & /*unused*/) noexcept
                      -> InvokeStatus<void> {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  });
    auto contract_0 = std::make_unique<NativeContract>(std::move(fns_0));

    // Invokes another contract, returns the error code of the other contract
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_1;
    fns_1.emplace(
        0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
          auto const &calldata = ctx.handler().get_calldata();

          if (calldata.size() !=
              sizeof(Runtime::MethodID) + sizeof(ContractAddress)) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          Runtime::MethodID invoked_id =
              *reinterpret_cast<const Runtime::MethodID *>(calldata.data());
          ContractAddress invoked_addr;
          std::memcpy(invoked_addr.data(),
                      calldata.data() + sizeof(Runtime::MethodID),
                      invoked_addr.size());

          CopyableInvokeInputs inputs{
              .entrypoint_id = invoked_id,
              .gas_limit = std::numeric_limits<GasAmount>::max(),
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto invoke_res =
              ctx.syscall<SyscallNo::kInvoke>(invoked_addr,
                                              std::span<const std::byte>{},
                                              std::span<uint32_t>{}, inputs,
                                              CopyableInvokeOutputs{});

          if (invoke_res) {
            return (invoke_res.value() ==
                    SyscallErrorCode::kSyscallErrorCodeNone)
                       ? InvokeStatus<void>{}
                       : InvokeError{InvokeErrorCode::Deterministic};
          }
          return InvokeError{InvokeErrorCode::Deterministic};
        });
    auto contract_1 = std::make_unique<NativeContract>(std::move(fns_1));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(contract_0));
    contract_map.emplace(1, std::move(contract_1));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };
  auto context = make_context();

  Transaction tx{};
  tx.contracts_to_deploy.push_back(contract);

  SystemCallHandler handler = make_handler(tx, snapshot, 0, 0, *context);

  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size = 0,
      .contract_type = ContractType::kNative,
  };

  auto runtime_res =
      context->make_new_runtime(deployed_contract_hash, &handler);
  ASSERT_TRUE(runtime_res.has_value());
  auto runtime = std::move(runtime_res.value());
  ASSERT_TRUE(runtime != nullptr);

  auto res = runtime->invoke(0, UINT64_MAX);

  ASSERT_TRUE(res.result_);

  res = runtime->invoke(1, UINT64_MAX);

  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::Deterministic);
}

namespace {

// Distinct deployment addresses for multi-contract tests that do not use
// NativeContractTestFramework::make_contract_address.
constexpr ContractAddress
test_contract_address(uint8_t tag)
{
  ContractAddress a{};
  a[0] = tag;
  return a;
}

} // namespace

TEST_F(NativeContractTest, GasCostsAccumulate)
{
  // Verifies that gas spent in nested invocations is charged to the root
  // transaction: a leaf contract consumes configurable gas; a middle contract
  // invokes the leaf three times (100, 200, 150) and checks each delta; an
  // entry contract invokes the middle once and checks that the nested total
  // (450) matches the gas consumed for that subtree. The three roles are
  // deployed as separate native contracts at separate addresses.

  constexpr static ContractAddress kAddressConsumer = test_contract_address(0);
  constexpr static ContractAddress kAddressMiddle = test_contract_address(1);
  constexpr static ContractAddress kAddressEntry = test_contract_address(2);

  NativeContractBuilder builder_consumer;
  builder_consumer.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0,
      [](NativeSyscallContext &runtime,
         uint32_t test_amount) -> InvokeStatus<int32_t> {
        if (!runtime.consume_gas(test_amount)) {
          return InvokeError{InvokeErrorCode::OutOfGas};
        }
        return 0;
      });

  NativeContractBuilder builder_middle;
  builder_middle.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<int32_t> {
        // Invoke consumer method 0 three times, with various gas costs
        auto r1 = [&]() {
          auto gas_before = ctx.gas_remaining();
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddressConsumer,
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<uint32_t>(100)
                      .value(),
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          return std::pair{r, gas_before - ctx.gas_remaining()};
        }();
        auto const &[r1_status, g1] = r1;
        if (!r1_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (g1 != 100) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r2 = [&]() {
          auto gas_before = ctx.gas_remaining();
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddressConsumer,
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<uint32_t>(200)
                      .value(),
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          return std::pair{r, gas_before - ctx.gas_remaining()};
        }();
        auto const &[r2_status, g2] = r2;
        if (!r2_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (g2 != 200) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r3 = [&]() {
          auto gas_before = ctx.gas_remaining();
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kAddressConsumer,
              Serializer<NativeContractBuilder::kDefaultSerializationFormat>::
                  serialize<uint32_t>(150)
                      .value(),
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
          return std::pair{r, gas_before - ctx.gas_remaining()};
        }();
        auto const &[r3_status, g3] = r3;
        if (!r3_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (g3 != 150) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return 0;
      });

  NativeContractBuilder builder_entry;
  builder_entry.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<int32_t> {
        auto r = [&]() {
          auto gas_before = ctx.gas_remaining();
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto invoke_status =
              ctx.syscall<SyscallNo::kInvoke>(kAddressMiddle,
                                              std::span<const std::byte>{},
                                              std::span<uint32_t>{}, inputs,
                                              CopyableInvokeOutputs{});
          return std::pair{invoke_status, gas_before - ctx.gas_remaining()};
        }();
        auto const &[invoke_status, g] = r;
        if (!invoke_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (g != 100 + 200 + 150) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return 0;
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder_consumer.build());
  contract_map.emplace(1, builder_middle.build());
  contract_map.emplace(2, builder_entry.build());

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  ContractAddress const deployment_addrs[] = {kAddressConsumer, kAddressMiddle,
                                              kAddressEntry};
  for (int32_t i = 0; i < 3; ++i) {
    Contract contract(ContractType::kNative);
    contract.native_contract_id() =
        static_cast<HogexecPrecompiledNativeContracts>(i);
    snapshot_server_->impl()->setup_create_contract(contract);
    snapshot_server_->impl()->setup_create_deployment(
        deployment_addrs[i],
        DeployedContractHash{.hash = sha256(contract),
                             .contract_size = 0,
                             .contract_type = contract.contract_type()});
  }

  SystemCallHandler handler = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address = kAddressEntry,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = UINT64_MAX,
                  .calldata = {},
              },
      },
      snapshot, 0, 0, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(res.gas_consumed_, 100 + 200 + 150);
}

TEST_F(NativeContractTest, GasCostsAtBoundary)
{
  // Verifies boundary gas accounting for native invokes: gas_limit 256 permits
  // a child that consumes 255 gas, while gas_limit 255 is converted to
  // OutOfGas/DeterministicWasm and still charges the full child limit.

  constexpr static ContractAddress kAddressConsumer = test_contract_address(0);
  constexpr static ContractAddress kAddressEntry = test_contract_address(1);

  NativeContractBuilder builder_consumer;
  builder_consumer.add<SerializationFormat::kTxs>(
      0,
      [](NativeSyscallContext &runtime,
         uint32_t test_amount) -> InvokeStatus<int32_t> {
        if (!runtime.consume_gas(test_amount)) {
          return InvokeError{InvokeErrorCode::OutOfGas};
        }
        return 0;
      });

  NativeContractBuilder builder_entry;
  builder_entry.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<int32_t> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = 256,
            .flags = std::to_underlying(InvokeFlag::None),
        };
        auto invoke_status =
            ctx.syscall<SyscallNo::kInvoke>(kAddressConsumer,
                                            Calldata{0x00, 0x00, 0x00, 0xFF},
                                            std::span<uint32_t>{}, inputs,
                                            CopyableInvokeOutputs{});
        if (!invoke_status.has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = 255,
            .flags = std::to_underlying(InvokeFlag::None),
        };
        invoke_status =
            ctx.syscall<SyscallNo::kInvoke>(kAddressConsumer,
                                            Calldata{0x00, 0x00, 0x00, 0xFF},
                                            std::span<uint32_t>{}, inputs2,
                                            CopyableInvokeOutputs{});
        if (!invoke_status.has_value() ||
            invoke_status.value() !=
                SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, builder_consumer.build());
  contract_map.emplace(1, builder_entry.build());

  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  ContractAddress const deployment_addrs[] = {kAddressConsumer, kAddressEntry};
  for (int32_t i = 0; i < 2; ++i) {
    Contract contract(ContractType::kNative);
    contract.native_contract_id() =
        static_cast<HogexecPrecompiledNativeContracts>(i);
    snapshot_server_->impl()->setup_create_contract(contract);
    snapshot_server_->impl()->setup_create_deployment(
        deployment_addrs[i],
        DeployedContractHash{.hash = sha256(contract),
                             .contract_size = 0,
                             .contract_type = contract.contract_type()});
  }

  SystemCallHandler handler = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address = kAddressEntry,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = UINT64_MAX,
                  .calldata = {},
              },
      },
      snapshot, 0, 0, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.result_);
  ASSERT_EQ(res.gas_consumed_, 510);
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
  StorageKey bad_full_key(StorageKeyType::ContractData);
  bad_full_key.contract_data_key() = kBadKey;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto bstring = make_bytestring("bad");
        // comment this set_bytestring to verify that the test framework
        // does, in fact, throw errors if you get_bytestring from the snapshot
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), kBadKey, std::as_bytes(std::span(bstring)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // This would have failed if the bytestring was not loaded lazily:
        // we're getting the bytestring from the prior write, not the snapshot
        auto r2 = ctx.handler().get_bytestring(kBadKey);
        if (!r2.has_value() || !r2.value().has_value()) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        assert(r2.value().value());
        if (*r2.value().value() != bstring) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        std::vector<uint8_t> bstring(100);
        RuntimeMemoryLength written_len = 0;
        auto res = ctx.syscall<SyscallNo::kBytestringGet>(
            kBadKey, std::as_writable_bytes(std::span(bstring)), written_len);
        // This test should fail because it accesses the bytestring the server
        // is configured to return an unrecoverable error on
        if (res.has_value() ||
            res.error().code() != HostFnErrorCode::Unrecoverable) {
          return InvokeError{InvokeErrorCode::Deterministic};
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
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0);

  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      address, DeployedContractHash{.hash = sha256(contract),
                                    .contract_size = 0,
                                    .contract_type = contract.contract_type()});
  snapshot_server_->impl()->setup_ban_item(
      AddressAndKey{.address = address, .key = bad_full_key});

  SystemCallHandler handler = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address = address,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = UINT64_MAX,
                  .calldata = {},
              },

      },
      snapshot, 0, 0, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  SystemCallHandler handler2 = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address = address,
                  .entrypoint = entrypoint_from_method_id(1),
                  .gas_limit = UINT64_MAX,
                  .calldata = {},
              },
      },
      snapshot, 0, 0, *runtime_context);
  res = handler2.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

TEST_F(NativeContractTest, LazyLoadingNNInt)
{
  // Test to verify that items are loaded lazily
  ContractAddress address{};
  constexpr static StorageKeyContractData kBadKey{};

  StorageKey bad_full_key(StorageKeyType::ContractData);
  bad_full_key.contract_data_key() = kBadKey;

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    NativeContractBuilder builder;

    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          // get_nnint() with force_load_from_snapshot=false from clean state
          // should not query db
          auto r0 = ctx.handler().get_nnint(kBadKey, false);
          if (!r0.has_value() || !r0.value().has_value() ||
              r0.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // add ops shouldn't query db
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), kBadKey, nullptr, 100,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          auto [r2, _gas2] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), kBadKey, nullptr, -200,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r2.has_value() ||
              r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // assert that get_nnint() without a load doesn't query db
          auto r3 = ctx.handler().get_nnint(kBadKey, false);
          if (!r3.has_value() || !r3.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r3.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });

    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          int128_t set_value = 100;
          auto [r, _gas] = syscall_NonnegativeIntSetAdd_inner(
              ctx.handler(), kBadKey, &set_value, 200,
              std::to_underlying(StorageDeltaFlags::None));
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // assert that get_nnint() after a set doesn't query db, even with
          // force_load_from_snapshot=true
          auto r2 = ctx.handler().get_nnint(kBadKey, true);
          if (!r2.has_value() || !r2.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!r2.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }

          // get_nnint() after a set with force_load_from_snapshot=false should
          // return the set value if it exists
          auto r3 = ctx.handler().get_nnint(kBadKey, false);
          if (!r3.has_value() || !r3.value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!r3.value().value().first.has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
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
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0);

  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      address, DeployedContractHash{.hash = sha256(contract),
                                    .contract_size = 0,
                                    .contract_type = contract.contract_type()});
  snapshot_server_->impl()->setup_ban_item(
      AddressAndKey{.address = address, .key = bad_full_key});

  {
    SystemCallHandler handler = make_handler(
        Transaction{
            .invocation =
                {
                    .invoked_address = address,
                    .entrypoint = entrypoint_from_method_id(0),
                    .gas_limit = UINT64_MAX,
                    .calldata = {},
                },
        },
        snapshot, 0, 0, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
  }
  {
    SystemCallHandler handler = make_handler(
        Transaction{
            .invocation =
                {
                    .invoked_address = address,
                    .entrypoint = entrypoint_from_method_id(1),
                    .gas_limit = UINT64_MAX,
                    .calldata = {},
                },
        },
        snapshot, 0, 1, *context);

    auto res = handler.root_invoke();
    ASSERT_TRUE(res.result_);
  }
}

// Regression test for issue #1512. An attached native contract with an
// unknown native_contract_id must be rejected by verify_contract rather
// than reaching the panic in the deploy path.
TEST_F(NativeContractTest, InvalidNativeContractIdRejectedByVerifyContract)
{
  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract malicious_contract(ContractType::kNative);
  malicious_contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0x7fffffff);

  auto make_context = []() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0,
                [](NativeSyscallContext & /*unused*/) noexcept
                    -> InvokeStatus<void> { return {}; });
    auto deployer_contract = std::make_unique<NativeContract>(std::move(fns));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(deployer_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };
  auto context = make_context();

  static constexpr ContractAddress kDeployerAddr = test_contract_address(99);
  Contract deployer_contract(ContractType::kNative);
  deployer_contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0);

  snapshot_server_->impl()->setup_create_contract(deployer_contract);
  snapshot_server_->impl()->setup_create_deployment(
      kDeployerAddr,
      DeployedContractHash{.hash = sha256(deployer_contract),
                           .contract_size = 0,
                           .contract_type = ContractType::kNative});

  SystemCallHandler handler = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address = kDeployerAddr,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = UINT64_MAX,
                  .calldata = {},
              },
          .contracts_to_deploy = {malicious_contract},
      },
      snapshot, 0, 0, *context);

  auto res = handler.root_invoke();
  ASSERT_FALSE(res.result_);
  ASSERT_EQ(res.result_.error(), InvokeErrorCode::Deterministic);
}

TEST_F(NativeContractTest, ManualRevertTryInvokeDoesNotProbeLocks)
{
  constexpr ContractAddress kAddrParent = test_contract_address(30);
  constexpr ContractAddress kAddrChild = test_contract_address(31);
  constexpr StorageKeyContractData kBannedDataKey{};

  StorageKey banned_full_key(StorageKeyType::ContractData);
  banned_full_key.contract_data_key() = kBannedDataKey;
  AddressAndKey banned_addrkey{.address = kAddrChild, .key = banned_full_key};

  // Ban the child's key in the subtransaction path — any lock probe touching
  // this key will return kRPCUnrecoverable.
  snapshot_server_->impl()->setup_ban_subtransaction_key(banned_addrkey);

  // Parent: TryInvoke the child, assert FrameReverted.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [kAddrChild](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = ctx.gas_remaining(),
            .flags = std::to_underlying(InvokeFlag::TryInvoke),
        };
        auto r = ctx.syscall<SyscallNo::kInvoke>(kAddrChild,
                                                 std::span<const std::byte>{},
                                                 std::span<uint32_t>{}, inputs,
                                                 CopyableInvokeOutputs{});
        if (!r.has_value()) {
          ADD_FAILURE()
              << "TryInvoke returned system error (lock probe reached "
                 "banned key?)";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (r.value() != SyscallErrorCode::kSyscallErrorCodeFrameReverted) {
          ADD_FAILURE() << "Expected FrameReverted, got "
                        << static_cast<int32_t>(r.value());
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto contract_parent =
      std::make_unique<NativeContract>(std::move(fns_parent));

  // Child: write to the banned key, then manually revert.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto data = make_bytestring("child_write");
        constexpr StorageKeyContractData kKey{};
        auto [res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), kKey, std::as_bytes(std::span(data)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!res.has_value() ||
            res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        ctx.handler().set_return_buffer(ReturnBuffer{}, true);
        return {};
      });
  auto contract_child = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract_parent));
  contract_map.emplace(1, std::move(contract_child));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);

  Contract parent_contract(ContractType::kNative);
  parent_contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0);
  Contract child_contract(ContractType::kNative);
  child_contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(1);

  snapshot_server_->impl()->setup_create_contract(parent_contract);
  snapshot_server_->impl()->setup_create_deployment(
      kAddrParent,
      DeployedContractHash{.hash = sha256(parent_contract),
                           .contract_size = 0,
                           .contract_type = parent_contract.contract_type()});

  snapshot_server_->impl()->setup_create_contract(child_contract);
  snapshot_server_->impl()->setup_create_deployment(
      kAddrChild,
      DeployedContractHash{.hash = sha256(child_contract),
                           .contract_size = 0,
                           .contract_type = child_contract.contract_type()});

  SystemCallHandler handler =
      make_handler(Transaction{.invocation =
                                   Invocation{.invoked_address = kAddrParent,
                                              .entrypoint =
                                                  entrypoint_from_method_id(0),
                                              .gas_limit = UINT64_MAX,
                                              .calldata = {}}},
                   snapshot, 0, 0, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

TEST_F(NativeContractTest, OversizedCalldataDoesNotThrowBadAlloc)
{
  StorageSnapshotCache snapshot(0, *db_proxy_);
  auto runtime_context = make_hogexec_precompiles_context();

  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      HogexecPrecompiledNativeContracts::kNativeContractInitialTokenClaim;
  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      GenesisConfigurationLoader::kDeployContractAddress,
      DeployedContractHash{.hash = sha256(contract),
                           .contract_size = 0,
                           .contract_type = contract.contract_type()});

  // Forge calldata: valid SHA256Hash + SHA256Hash + ContractEntrypointID,
  // then a Calldata field whose CXS length header claims 32 MiB but
  // the buffer is only a few bytes longer than the header.
  using itc_meta = ContractTraitMeta<initial_token_claim::InitialTokenClaim>;

  SHA256Hash hash{};
  SHA256Hash nonce{};
  ContractEntrypointID entrypoint{};

  // Serialize the fixed-size args as valid CXS prefix.
  // For fixed-size opaque arrays, CXS serialization is raw bytes.
  auto prefix =
      Serializer<itc_meta::kSerializationFormat>::serialize(hash, nonce,
                                                            entrypoint);
  ASSERT_TRUE(prefix.has_value());

  // Append a CXS-encoded length claiming 32 MiB.
  // CXS compact-uint encoding for a uint32 value:
  //   header byte 0xBF (kCxsHeader4Byte) + 4 bytes little-endian value.
  constexpr uint32_t kFakeSize = 32 * 1024 * 1024;
  uint8_t cxs_len_bytes[5];
  cxs_len_bytes[0] = 0xBF; // CXS header for 4-byte unsigned integer
  std::memcpy(cxs_len_bytes + 1, &kFakeSize, sizeof(kFakeSize));

  Calldata forged;
  forged.append_range(
      prefix.value()); // NOLINT(bugprone-unchecked-optional-access)
  forged.append(std::begin(cxs_len_bytes), std::end(cxs_len_bytes));
  // Don't append the claimed 32 MiB — the buffer is truncated

  SystemCallHandler handler = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address =
                      GenesisConfigurationLoader::kDeployContractAddress,
                  .entrypoint = itc_meta::entrypoint<"claim_initial_tokens">(),
                  .gas_limit = UINT64_MAX,
                  .calldata = std::move(forged),
              },
      },
      snapshot, 0, 0, *runtime_context);

  // This must NOT throw std::bad_alloc. The zero-copy BufView
  // deserialization path validates the declared size against the
  // actual input buffer via check_size(), which throws CxsBadInput
  // (caught by serialize_calldata.h), returning a deterministic error.
  EXPECT_NO_THROW({
    auto res = handler.root_invoke();
    // Should be a deterministic error (deserialization failure), not a crash
    EXPECT_FALSE(res.result_.has_value());
  });
}

// A contract that supplies a buffer shorter than the stored value must be told
// the FULL length of the value (so it can detect truncation), while only
// buffer-sized bytes are copied and bytes past the buffer are left untouched.
TEST_F(NativeContractTest, BytestringGetReportsFullLengthOnShortBuffer)
{
  constexpr static StorageKeyContractData kKey{};

  NativeContractBuilder builder;
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
        auto value = make_bytestring("12345678"); // 8 bytes
        auto [set_res, _gas] = syscall_BytestringSet_inner(
            ctx.handler(), kKey, std::as_bytes(std::span(value)),
            std::to_underlying(StorageDeltaFlags::None));
        if (!set_res.has_value() ||
            set_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "BytestringSet failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Read the 8-byte value into a 4-byte buffer (with a sentinel byte
        // just past the buffer to detect overruns).
        std::array<uint8_t, 5> backing;
        backing.fill(0xEE);
        std::span<std::byte> short_buf =
            std::as_writable_bytes(std::span(backing).first(4));
        RuntimeMemoryLength written_len = 0;
        auto get_res =
            ctx.syscall<SyscallNo::kBytestringGet>(kKey, short_buf, written_len);
        if (!get_res.has_value() ||
            get_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "BytestringGet failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (written_len != 8) {
          ADD_FAILURE() << "expected full length 8, got " << written_len;
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (backing[0] != '1' || backing[3] != '4') {
          ADD_FAILURE() << "buffer prefix not copied";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (backing[4] != 0xEE) {
          ADD_FAILURE() << "byte past buffer was overwritten";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });

  auto native_contract = builder.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  StorageSnapshotCache snapshot(0, *db_proxy_);
  ContractAddress address{};
  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      static_cast<HogexecPrecompiledNativeContracts>(0);
  snapshot_server_->impl()->setup_create_contract(contract);
  snapshot_server_->impl()->setup_create_deployment(
      address, DeployedContractHash{.hash = sha256(contract),
                                    .contract_size = 0,
                                    .contract_type = contract.contract_type()});

  SystemCallHandler handler = make_handler(
      Transaction{
          .invocation =
              {
                  .invoked_address = address,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = UINT64_MAX,
                  .calldata = {},
              },
      },
      snapshot, 0, 0, *runtime_context);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
