#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/hog_exec/runtime/syscall_meta.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "network_config.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class Sha256IncrementalTest : public ::testing::Test,
                              public NativeContractTestFramework {
public:
  Sha256IncrementalTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true)
  {}
};

// Exact gas exhaustion returns OutOfGas before hashing.
TEST_F(Sha256IncrementalTest, OneShotChecksGasBeforeHashing)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);
  ASSERT_TRUE(
      handler
          .start_frame(Invocation{.invoked_address = make_contract_address(0),
                                  .entrypoint = {},
                                  .gas_limit = UINT64_MAX,
                                  .calldata = {}},
                       std::to_underlying(InvokeFlag::None),
                       ValidatedAuthTokens{})
          .has_value());
  handler.enable_transaction_stats();
  for (uint32_t size : {0U, 55U, 56U, 63U, 64U, 65U, 4097U}) {
    // The body itself leaves its output untouched, including when hashing an
    // empty input (whose byte counter alone cannot detect work).
    const std::vector<std::byte> input(size, std::byte{0xab});
    SHA256Hash digest;
    digest.fill(0xab);
    const auto sentinel = digest;
    auto [inner_result, inner_charge] =
        syscall_SHA256_inner(handler,
                             WrappedGasAmount{GasCost::sha256_hash(size)},
                             input, digest);
    ASSERT_FALSE(inner_result.has_value());
    EXPECT_EQ(inner_result.error().code(), HostFnErrorCode::OutOfGas);
    EXPECT_EQ(inner_charge.get(), GasCost::sha256_hash(size));
    EXPECT_EQ(digest, sentinel);
    const GasAmount cost = kSyscallBaseCost + GasCost::sha256_hash(size);
    for (GasAmount budget :
         {GasAmount{0}, kSyscallBaseCost, cost - 1, cost, cost + 1}) {
      SCOPED_TRACE(::testing::Message()
                   << "size=" << size << " budget=" << budget);
      std::vector<std::byte> memory(size + sizeof(SHA256Hash), std::byte{0xab});

      handler.get_transaction_stats()->sha256_total_bytes_processed = 0;
      auto [result, charged] = wrap_outer<SyscallMeta<SyscallNo::kSHA256>>(
          handler, ExecutionContext{.memory = memory, .gas_remaining = budget},
          SyscallNo::kSHA256, 0, size, size, 0, 0, 0, 0)();
      if (budget <= cost) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), HostFnErrorCode::OutOfGas);
        EXPECT_EQ(charged, budget);
        // WritableOutput flushes its zero-initialized staging value on a
        // frame fault; the digest itself must never be computed.
        EXPECT_EQ(std::vector<std::byte>(memory.begin() + size, memory.end()),
                  std::vector<std::byte>(sizeof(SHA256Hash), std::byte{0}));
        EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
                  0);
      }
      else {
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(charged, cost);
        const auto expected = sha256(std::span(memory).first(size));
        EXPECT_EQ(
            std::vector<std::byte>(memory.begin() + size, memory.end()),
            std::vector<std::byte>(std::as_bytes(std::span(expected)).begin(),
                                   std::as_bytes(std::span(expected)).end()));
        EXPECT_EQ(handler.get_transaction_stats()->sha256_total_bytes_processed,
                  size);
      }
    }
  }
  EXPECT_TRUE(
      handler.finish_frame(InvokeStatus<void>{}, nullptr)().has_value());
}

TEST_F(Sha256IncrementalTest, UpdateChecksGasBeforeCreatingResettingOrAppending)
{
  auto context = make_hog_exec_context();
  auto handler = make_new_system_call_handler(Transaction{}, *context);
  ASSERT_TRUE(
      handler
          .start_frame(Invocation{.invoked_address = make_contract_address(0),
                                  .entrypoint = {},
                                  .gas_limit = UINT64_MAX,
                                  .calldata = {}},
                       std::to_underlying(InvokeFlag::None),
                       ValidatedAuthTokens{})
          .has_value());
  handler.enable_transaction_stats();
  const std::array<std::byte, 3> prefix{std::byte{1}, std::byte{2},
                                        std::byte{3}};
  for (uint32_t size : {0U, 63U, 64U, 65U, 4097U}) {
    const GasAmount cost = kSyscallBaseCost + GasCost::sha256_update(size);
    for (bool existing : {false, true}) {
      for (bool reset : {false, true}) {
        for (GasAmount budget :
             {GasAmount{0}, kSyscallBaseCost, cost - 1, cost, cost + 1}) {
          SCOPED_TRACE(::testing::Message()
                       << "size=" << size << " existing=" << existing
                       << " reset=" << reset << " budget=" << budget);
          if (existing) {
            ASSERT_TRUE(handler.sha256_update(0, prefix, true).has_value());
          }
          handler.get_transaction_stats()->sha256_total_bytes_processed = 0;
          std::vector<std::byte> memory(size, std::byte{0xab});
          auto [result, charged] =
              wrap_outer<SyscallMeta<SyscallNo::kSHA256Update>>(
                  handler,
                  ExecutionContext{.memory = memory, .gas_remaining = budget},
                  SyscallNo::kSHA256Update, 0, 0, size,
                  static_cast<uint32_t>(reset), 0, 0, 0)();
          if (budget <= cost) {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), HostFnErrorCode::OutOfGas);
            EXPECT_EQ(charged, budget);
            EXPECT_EQ(
                handler.get_transaction_stats()->sha256_total_bytes_processed,
                0);
            auto digest = handler.sha256_finalize(0);
            EXPECT_EQ(digest.has_value(), existing);
            if (existing) {
              ASSERT_TRUE(digest.has_value());
              EXPECT_EQ(*digest, sha256(prefix));
            }
          }
          else if (!existing && !reset) {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(*result,
                      SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
            EXPECT_EQ(charged, kSyscallBaseCost);
            EXPECT_FALSE(handler.sha256_finalize(0).has_value());
          }
          else {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeNone);
            EXPECT_EQ(charged, cost);
            auto digest = handler.sha256_finalize(0);
            ASSERT_TRUE(digest.has_value());
            if (existing && !reset) {
              memory.insert(memory.begin(), prefix.begin(), prefix.end());
            }
            EXPECT_EQ(*digest, sha256(memory));
            // Finalize must not count bytes already processed by Update again.
            EXPECT_EQ(
                handler.get_transaction_stats()->sha256_total_bytes_processed,
                size);
          }
        }
      }
    }
  }
  EXPECT_TRUE(
      handler.finish_frame(InvokeStatus<void>{}, nullptr)().has_value());
}

TEST_F(Sha256IncrementalTest, BasicUpdateAndFinalize)
{
  NativeContractBuilder builder;
  constexpr static ContractAddress kAddress =
      NativeContractTestFramework::make_contract_address(0);
  constexpr static int32_t kContractId = 0;

  // Simple incremental hash in a single invocation frame.
  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::array<uint8_t, 4> first_chunk = {0, 1, 2, 3};
        const std::array<uint8_t, 4> second_chunk = {4, 5, 6, 7};

        auto r1 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(first_chunk)), true);
        if (!r1.has_value() ||
            r1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto r2 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(second_chunk)), false);
        if (!r2.has_value() ||
            r2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash digest{};
        auto r3 = co_await ctx.syscall<SyscallNo::kSHA256Finalize>(0, digest);
        if (!r3.has_value() ||
            r3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        const auto expected_hash =
            sha256(std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7});
        if (digest != expected_hash) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto native_contract = builder.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kContractId, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(kContractId);
  write_contract(contract_xdr);
  write_deployment(kAddress, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.result_);
}

TEST_F(Sha256IncrementalTest, FramesKeepIndependentHashBuffers)
{
  constexpr static ContractAddress kParentAddress =
      NativeContractTestFramework::make_contract_address(1);
  constexpr static ContractAddress kChildAddress =
      NativeContractTestFramework::make_contract_address(2);
  constexpr static int32_t kParentContractId = 1;
  constexpr static int32_t kChildContractId = 2;

  // Parent updates buffer 0, invokes a separate child contract that also uses
  // buffer 0, then continues hashing to confirm isolation between frames.
  NativeContractBuilder builder_parent;
  builder_parent.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::array<uint8_t, 3> parent_prefix = {'p', 'a', 'r'};
        const std::array<uint8_t, 3> parent_suffix = {'e', 'n', 't'};

        auto p1 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(parent_prefix)), true);
        if (!p1.has_value() ||
            p1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto invoke_status =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              kChildAddress, std::span<const std::byte>{},
              std::span<uint32_t>{}, inputs, CopyableInvokeOutputs{});
        }();
        if (!invoke_status.has_value() ||
            invoke_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto p2 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(parent_suffix)), false);
        if (!p2.has_value() ||
            p2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash digest{};
        auto p3 = co_await ctx.syscall<SyscallNo::kSHA256Finalize>(0, digest);
        if (!p3.has_value() ||
            p3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        const auto expected_parent_hash =
            sha256(std::vector<uint8_t>{'p', 'a', 'r', 'e', 'n', 't'});
        if (digest != expected_parent_hash) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  NativeContractBuilder builder_child;
  builder_child.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::array<uint8_t, 3> child_prefix = {'c', 'h', 'i'};
        const std::array<uint8_t, 3> child_suffix = {'l', 'd', '1'};

        auto c1 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(child_prefix)), true);
        if (!c1.has_value() ||
            c1.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto c2 = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            0, std::as_bytes(std::span(child_suffix)), false);
        if (!c2.has_value() ||
            c2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        SHA256Hash digest{};
        auto c3 = co_await ctx.syscall<SyscallNo::kSHA256Finalize>(0, digest);
        if (!c3.has_value() ||
            c3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        const auto expected_child_hash =
            sha256(std::vector<uint8_t>{'c', 'h', 'i', 'l', 'd', '1'});
        if (digest != expected_child_hash) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto native_contract_parent = builder_parent.build();
  auto native_contract_child = builder_child.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kParentContractId, std::move(native_contract_parent));
  contract_map.emplace(kChildContractId, std::move(native_contract_child));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr_parent = make_native_contract(kParentContractId);
  write_contract(contract_xdr_parent);
  write_deployment(kParentAddress, contract_xdr_parent);

  auto contract_xdr_child = make_native_contract(kChildContractId);
  write_contract(contract_xdr_child);
  write_deployment(kChildAddress, contract_xdr_child);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kParentAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.result_);
}

// A chain that configures kConfigParamMaxSHA256Buffers away from its default
// must actually execute under that value: the parameter travels from
// GenesisConfiguration through the DbTip to the syscall handler, and a break
// anywhere on that path would leave the compiled-in default in force.
class Sha256ConfiguredBufferLimitTest : public ::testing::Test,
                                        public NativeContractTestFramework {
public:
  static constexpr uint32_t kConfiguredMaxBuffers = 2;

  Sha256ConfiguredBufferLimitTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true, std::nullopt, genesis_network_config())
  {}

private:
  static NetworkConfig genesis_network_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxSHA256Buffers,
                                   kConfiguredMaxBuffers);
    return config;
  }
};

TEST_F(Sha256ConfiguredBufferLimitTest, GenesisValueBoundsOpenBuffers)
{
  NativeContractBuilder builder;
  constexpr static ContractAddress kAddress =
      NativeContractTestFramework::make_contract_address(0);
  constexpr static int32_t kContractId = 0;

  builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::array<uint8_t, 1> chunk = {0};

        // The configured number of buffers opens; the stock value of 32 would
        // let this run well past kConfiguredMaxBuffers.
        for (uint32_t i = 0; i < kConfiguredMaxBuffers; i++) {
          auto opened = co_await ctx.syscall<SyscallNo::kSHA256Update>(
              i, std::as_bytes(std::span(chunk)), true);
          if (!opened.has_value() ||
              opened.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        auto beyond = co_await ctx.syscall<SyscallNo::kSHA256Update>(
            kConfiguredMaxBuffers, std::as_bytes(std::span(chunk)), true);
        if (!beyond.has_value() ||
            beyond.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });

  auto native_contract = builder.build();
  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(kContractId, std::move(native_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(kContractId);
  write_contract(contract_xdr);
  write_deployment(kAddress, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = kAddress,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *runtime_context);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.result_);
}

} // namespace hogd
