#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "auth_token.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hog_exec_constants.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "network_config.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

// Far below the protocol defaults, so a test passing here shows the
// configured value is what is enforced rather than the constant.
constexpr uint32_t kSavepointDepth = 4;
constexpr uint32_t kAuthTokenDepth = 2;
static_assert(kSavepointDepth < kMaxSavepointDepth);
static_assert(kAuthTokenDepth < kAuthTokenMaxDepth);

constexpr ContractAddress kAddressNoop =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kAddressDriver =
    NativeContractTestFramework::make_contract_address(1);

// One savepoint slot is reserved for the root commit, so the configured depth
// admits exactly this many child TryInvokes.
constexpr uint32_t kChildCap = kSavepointDepth - 1;

// A chain of `depth` nested AuthTokenNodes.
AuthTokenNode
make_token_tree(ContractAddress const &consumer, uint32_t depth)
{
  AuthTokenNode node{.consumer_address = consumer,
                     .payload = x23::serialize(depth),
                     .child_nodes = {}};
  for (uint32_t level = depth; level > 1; --level) {
    node = AuthTokenNode{.consumer_address = consumer,
                         .payload = x23::serialize(level - uint32_t{1}),
                         .child_nodes = {node}};
  }
  return node;
}

} // namespace

// The execution limits a chain states at genesis are what the executor
// enforces: these run under a configuration far below the protocol defaults
// and pin that the configured value, not the constant, is the boundary.
class ExecutionLimitsTest : public ::testing::TestWithParam<TestDB>,
                            public NativeContractTestFramework {
protected:
  ExecutionLimitsTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam(),
          true, std::nullopt, lowered_network_config())
  {}

  void SetUp() override
  {
    for (auto const &addr : {kAddressNoop, kAddressDriver}) {
      setup_grant_storage_token(addr, 1'000'000'000);
    }
  }

  // Deploys `noop` at kAddressNoop and `driver` at kAddressDriver, invokes the
  // driver as the root, and reports whether the root succeeded.
  bool run_driver(NativeContract::function_t driver)
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_noop;
    fns_noop.emplace(
        0, [](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
          co_return InvokeStatus<void>{};
        });
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_driver;
    fns_driver.emplace(0, std::move(driver));

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0,
                         std::make_unique<NativeContract>(std::move(fns_noop)));
    contract_map.emplace(1, std::make_unique<NativeContract>(
                                std::move(fns_driver)));
    auto context = std::make_unique<NativeContext>(std::move(contract_map));

    ContractAddress const addrs[] = {kAddressNoop, kAddressDriver};
    for (int i = 0; i < 2; ++i) {
      auto c = make_native_contract(i);
      write_contract(c);
      write_deployment(addrs[i], c);
    }

    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = kAddressDriver,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);
    return handler.root_invoke()().is_root_success();
  }

private:
  static NetworkConfig lowered_network_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxSavepointDepth,
                                   kSavepointDepth);
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxAuthTokenDepth,
                                   kAuthTokenDepth);
    return config;
  }
};

// One savepoint slot is reserved for the root commit, so the configured
// depth admits exactly depth - 1 child TryInvokes; the one after fails
// deterministically without the syscall itself erroring.
TEST_P(ExecutionLimitsTest, TryInvokeStopsAtConfiguredSavepointDepth)
{
  const bool ok = run_driver(
      [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        uint32_t successes = 0;
        for (uint32_t i = 0; i < kChildCap + 1; i++) {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddressNoop, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, outputs);
          if (!r.has_value()) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone &&
              outputs.frame_status == InvocationFrameStatusCode::Return) {
            successes++;
          }
        }
        if (successes != kChildCap) {
          ADD_FAILURE() << "expected " << kChildCap
                        << " committed TryInvokes, got " << successes;
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  EXPECT_TRUE(ok);
}

// A token at the configured depth mints; one level deeper is refused as
// invalid input, though the protocol default would have admitted it.
TEST_P(ExecutionLimitsTest, MintAuthTokenStopsAtConfiguredDepth)
{
  const bool ok = run_driver([](NativeSyscallContext &ctx)
                                 -> x23::Async<InvokeStatus<void>> {
    for (auto const &[depth, expected] :
         {std::pair{kAuthTokenDepth, SyscallErrorCode::kSyscallErrorCodeNone},
          std::pair{kAuthTokenDepth + 1,
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput}}) {
      auto serialized = x23::serialize(make_token_tree(kAddressDriver, depth));
      uint32_t token_id = 0;
      auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
          BlockHeight{1000},
          std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                         serialized.data()),
                                     serialized.size()},
          token_id);
      if (!mint_res.has_value() || mint_res.value() != expected) {
        ADD_FAILURE() << "depth " << depth << ": unexpected mint result";
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
    }
    co_return InvokeStatus<void>{};
  });
  EXPECT_TRUE(ok);
}

INSTANTIATE_TEST_SUITE_P(TestDBs, ExecutionLimitsTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

namespace {

// Far below the protocol default, and far above what the small transactions
// below carry, so only the bulk writer crosses it.
constexpr uint32_t kOutputLimit = 4096;
static_assert(kOutputLimit < kDefaultMaxSubtransactionOutputBytes);

// The bulk writer's storage output is kBulkWrites deltas of
// kStorageOutputBytesPerDelta + kBulkValueLen each, far past the limit.
constexpr size_t kBulkWrites = 40;
constexpr size_t kBulkValueLen = 128;
static_assert(kBulkWrites * (kStorageOutputBytesPerDelta + kBulkValueLen) >
              2 * kOutputLimit);

constexpr ContractAddress kAddressWriter =
    NativeContractTestFramework::make_contract_address(0);
constexpr ContractAddress kAddressCaller =
    NativeContractTestFramework::make_contract_address(1);

// Writer methods.
constexpr Runtime::MethodID kWriteBulk = 0;
constexpr Runtime::MethodID kWriteOne = 1;

x23::Async<InvokeStatus<void>>
write_bytestring(NativeSyscallContext &ctx, std::string const &key,
                 std::string const &value)
{
  auto r = co_await ctx.syscall<SyscallNo::kBytestringSet>(
      NativeContractTestFramework::make_storage_key(key),
      std::as_bytes(std::span(value)));
  if (!r.has_value() || r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
    co_return InvokeError{InvokeErrorCode::Deterministic};
  }
  co_return InvokeStatus<void>{};
}

std::string
bulk_key(size_t i)
{
  return "bulk" + std::to_string(i);
}

} // namespace

// max_subtransaction_output_bytes bounds each subtransaction a transaction
// pushes. Every case replays its proposal in validation mode and requires the
// same outcome and the same resulting state, since a root commit over the
// limit is a deterministic failure every node must agree on.
class SubtransactionOutputLimitTest : public ::testing::TestWithParam<TestDB>,
                                      public NativeContractTestFramework {
protected:
  SubtransactionOutputLimitTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam(),
          true, std::nullopt, lowered_network_config())
  {}

  void SetUp() override
  {
    for (auto const &addr : {kAddressWriter, kAddressCaller}) {
      setup_grant_storage_token(addr, 1'000'000'000);
    }
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, make_writer());
    contract_map.emplace(1, make_caller());
    context_ = std::make_unique<NativeContext>(std::move(contract_map));

    ContractAddress const addrs[] = {kAddressWriter, kAddressCaller};
    for (int i = 0; i < 2; ++i) {
      auto c = make_native_contract(i);
      write_contract(c);
      write_deployment(addrs[i], c);
    }
  }

  Transaction make_tx(ContractAddress const &address, Runtime::MethodID method)
  {
    return Transaction{.invocation =
                           Invocation{.invoked_address = address,
                                      .entrypoint =
                                          entrypoint_from_method_id(method),
                                      .gas_limit = UINT64_MAX,
                                      .calldata = {}},
                       .uniquifier = make_new_uniquifier()};
  }

  // Proposes `tx`, replays it in validation mode, and commits the validated
  // block. Expects `expected` from both and the same resulting state, and
  // returns the proposal's nondeterministic bit count.
  uint32_t propose_then_validate(Transaction const &tx, ExecResult expected)
  {
    auto proposer = make_new_system_call_handler(tx, *context_);
    EXPECT_TRUE(proposer.root_invoke()().is_root_success());
    auto proposal_data = proposer.get_proposal_data();
    EXPECT_EQ(proposer.try_commit_transaction()(), expected);
    const StorageShortHash proposed =
        discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

    auto validator =
        make_new_system_call_handler_validation(tx, *context_, proposal_data);
    EXPECT_TRUE(validator.root_invoke()().is_root_success());
    EXPECT_EQ(validator.try_commit_transaction()(), expected);
    EXPECT_EQ(advance_block(BlockExecutionMode::kBlockExecutionModeValidation),
              proposed)
        << "validation must reach the state the proposal did";
    return proposal_data.nondeterministic_bit_count;
  }

  bool has_item(ContractAddress const &address, std::string const &key)
  {
    return lookup_storage_item_from_db(make_contract_data_addrkey(address, key))
        .has_value();
  }

  std::unique_ptr<NativeContext> context_;

private:
  static NetworkConfig lowered_network_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxSubtransactionOutputBytes,
        kOutputLimit);
    return config;
  }

  static std::unique_ptr<NativeContract> make_writer()
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(kWriteBulk,
                [](NativeSyscallContext &ctx)
                    -> x23::Async<InvokeStatus<void>> {
                  const std::string value(kBulkValueLen, 'x');
                  for (size_t i = 0; i < kBulkWrites; ++i) {
                    auto r = co_await write_bytestring(ctx, bulk_key(i), value);
                    if (!r.has_value()) {
                      co_return r;
                    }
                  }
                  co_return InvokeStatus<void>{};
                });
    fns.emplace(kWriteOne,
                [](NativeSyscallContext &ctx)
                    -> x23::Async<InvokeStatus<void>> {
                  co_return co_await write_bytestring(ctx, "one", "value");
                });
    return std::make_unique<NativeContract>(std::move(fns));
  }

  // TryInvokes the bulk writer, requires that child to have been rolled back,
  // then writes a key of its own.
  static std::unique_ptr<NativeContract> make_caller()
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = kWriteBulk,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::TryInvoke),
          };
          CopyableInvokeOutputs outputs{};
          auto r = co_await ctx.syscall<SyscallNo::kInvoke>(
              kAddressWriter, std::span<const std::byte>{},
              std::span<uint32_t>{}, inputs, outputs);
          if (!r.has_value() ||
              r.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              outputs.frame_status == InvocationFrameStatusCode::Return) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return co_await write_bytestring(ctx, "after", "value");
        });
    return std::make_unique<NativeContract>(std::move(fns));
  }
};

// Positive control: a transaction within the limit commits as usual.
TEST_P(SubtransactionOutputLimitTest, WithinLimitCommits)
{
  propose_then_validate(make_tx(kAddressWriter, kWriteOne),
                        ExecResult::ExecResultOK);
  EXPECT_TRUE(has_item(kAddressWriter, "one"));
}

// A root commit over the limit fails deterministically rather than as a
// conflict: the transaction is included with its body discarded, so none of
// its writes land but its replay record does, and resubmitting it is refused
// as a replay.
TEST_P(SubtransactionOutputLimitTest, RootCommitOverLimitFailsDeterministically)
{
  const Transaction tx = make_tx(kAddressWriter, kWriteBulk);
  propose_then_validate(tx, ExecResult::ExecResultTxDeterministicError);
  EXPECT_FALSE(has_item(kAddressWriter, bulk_key(0)));
  EXPECT_FALSE(has_item(kAddressWriter, bulk_key(kBulkWrites - 1)));

  if (GetParam() == TestDB::kMockInMemory) {
    return; // MockInMemory does not support the replay cache.
  }
  auto replay = make_new_system_call_handler(tx, *context_);
  ASSERT_TRUE(replay.root_invoke()().is_root_success());
  EXPECT_EQ(replay.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError);
}

// A TryInvoke child over the limit fails as a lock conflict does: the child is
// rolled back, the proposal logs its bit, and the caller carries on.
TEST_P(SubtransactionOutputLimitTest, TryInvokeOverLimitRollsBackChild)
{
  const uint32_t bits = propose_then_validate(make_tx(kAddressCaller, 0),
                                              ExecResult::ExecResultOK);
  EXPECT_EQ(bits, 1U);
  EXPECT_FALSE(has_item(kAddressWriter, bulk_key(0)));
  EXPECT_TRUE(has_item(kAddressCaller, "after"));
}

INSTANTIATE_TEST_SUITE_P(TestDBs, SubtransactionOutputLimitTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
