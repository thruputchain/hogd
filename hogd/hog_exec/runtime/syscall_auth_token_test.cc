#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class SyscallAuthTokenTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  SyscallAuthTokenTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}

protected:
  void SetUp() override
  {
    // Every test in this fixture invokes the contract at
    // make_contract_address(0) as the root transaction and commits exactly
    // one transaction against it, consuming a replay-cache entry charged to
    // that address. Grant enough to cover that single commit.
    setup_grant_storage_token(make_contract_address(0),
                              replay_cache_cost_charged(1));
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(SyscallAuthTokenTest, InvokeForwardsOwnedAuthTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kMiddleAddress = make_contract_address(1);
  constexpr ContractAddress kLeafAddress = make_contract_address(2);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_leaf;
  fns_leaf.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto leaf_contract = std::make_unique<NativeContract>(std::move(fns_leaf));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_middle;
  fns_middle.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id calldata";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));
        std::array<uint32_t, 1> forwarded_token_ids{token_id};

        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kLeafAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded_token_ids}, inputs,
            CopyableInvokeOutputs{});
        if (!invoke_res.has_value() ||
            invoke_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Middle->Leaf invoke failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto middle_contract =
      std::make_unique<NativeContract>(std::move(fns_middle));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kMiddleAddress,
                                .payload = x23::serialize(7U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        if (!mint_res.has_value() ||
            mint_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "MintAuthToken failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        Calldata calldata;
        calldata.resize_uninitialized(sizeof(token_id));
        std::memcpy(calldata.data(), &token_id, sizeof(token_id));

        std::array<uint32_t, 1> owned_token_ids{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kMiddleAddress, calldata,
            std::span<uint32_t const>{owned_token_ids}, inputs,
            CopyableInvokeOutputs{});
        if (!invoke_res.has_value() ||
            invoke_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Caller->Middle invoke failed";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(middle_contract));
  contract_map.emplace(2, std::move(leaf_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto middle_xdr = make_native_contract(1);
  write_contract(middle_xdr);
  write_deployment(kMiddleAddress, middle_xdr);
  auto leaf_xdr = make_native_contract(2);
  write_contract(leaf_xdr);
  write_deployment(kLeafAddress, leaf_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, InvokeRejectsDuplicateForwardedAuthTokenIds)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        ADD_FAILURE() << "Child should not be invoked";
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(7U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 2> duplicate_token_ids{token_id, token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{duplicate_token_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ConsumeNestedAuthTokens)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode grandchild{.consumer_address = kAddr,
                                 .payload = x23::serialize(200U),
                                 .child_nodes = {}};
        AuthTokenNode child0{.consumer_address = kAddr,
                             .payload = x23::serialize(100U),
                             .child_nodes = {grandchild}};
        AuthTokenNode child1{.consumer_address = kAddr,
                             .payload = x23::serialize(300U),
                             .child_nodes = {}};
        AuthTokenNode root{.consumer_address = kAddr,
                           .payload = x23::serialize(42U),
                           .child_nodes = {child0, child1}};

        auto serialized = x23::serialize(root);
        uint32_t root_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            root_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Consume root -> payload=42, 2 children
        std::array<std::byte, 256> payload_buf{};
        uint32_t child_start = 0, child_count = 0;
        RuntimeMemoryLength payload_written_len = 0;
        ContractAddress minter{};
        auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            root_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, child_start, child_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(minter, kAddr);
        EXPECT_EQ(child_count, 2U);
        auto expected_root_payload = x23::serialize(42U);
        EXPECT_EQ(std::memcmp(payload_buf.data(), expected_root_payload.data(),
                              expected_root_payload.size()),
                  0);
        EXPECT_EQ(payload_written_len, expected_root_payload.size());

        // Consume child0 -> payload=100, 1 grandchild
        std::array<std::byte, 256> c0_payload{};
        RuntimeMemoryLength c0_payload_written_len = 0;
        uint32_t gc_start = 0, gc_count = 0;
        ContractAddress c0_minter{};
        auto c0_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            child_start, std::span<std::byte>{c0_payload}, c0_minter,
            c0_payload_written_len, gc_start, gc_count);
        EXPECT_TRUE(c0_res.has_value());
        EXPECT_EQ(c0_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(c0_minter, kAddr);
        EXPECT_EQ(gc_count, 1U);
        auto expected_c0_payload = x23::serialize(100U);
        EXPECT_EQ(std::memcmp(c0_payload.data(), expected_c0_payload.data(),
                              expected_c0_payload.size()),
                  0);

        // Consume child1 -> payload=300, 0 grandchildren
        std::array<std::byte, 256> c1_payload{};
        RuntimeMemoryLength c1_payload_written_len = 0;
        uint32_t unused_start = 0, c1_count = 0;
        ContractAddress c1_minter{};
        auto c1_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            child_start + 1, std::span<std::byte>{c1_payload}, c1_minter,
            c1_payload_written_len, unused_start, c1_count);
        EXPECT_TRUE(c1_res.has_value());
        EXPECT_EQ(c1_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(c1_minter, kAddr);
        EXPECT_EQ(c1_count, 0U);
        auto expected_c1_payload = x23::serialize(300U);
        EXPECT_EQ(std::memcmp(c1_payload.data(), expected_c1_payload.data(),
                              expected_c1_payload.size()),
                  0);
        EXPECT_EQ(c1_payload_written_len, expected_c1_payload.size());

        // Consume grandchild -> payload=200, 0 children
        std::array<std::byte, 256> gc_payload{};
        RuntimeMemoryLength gc_payload_written_len = 0;
        uint32_t gc_unused_start = 0, gc_gc_count = 0;
        ContractAddress gc_minter{};
        auto gc_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            gc_start, std::span<std::byte>{gc_payload}, gc_minter,
            gc_payload_written_len, gc_unused_start, gc_gc_count);
        EXPECT_TRUE(gc_res.has_value());
        EXPECT_EQ(gc_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(gc_minter, kAddr);
        EXPECT_EQ(gc_gc_count, 0U);
        auto expected_gc_payload = x23::serialize(200U);
        EXPECT_EQ(std::memcmp(gc_payload.data(), expected_gc_payload.data(),
                              expected_gc_payload.size()),
                  0);
        EXPECT_EQ(gc_payload_written_len, expected_gc_payload.size());

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

class SyscallAuthTokenExpiryTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  SyscallAuthTokenExpiryTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}

protected:
  void SetUp() override
  {
    // Every test in this fixture invokes the contract at
    // make_contract_address(0) as the root transaction and commits exactly
    // one transaction against it, consuming a replay-cache entry charged to
    // that address. Grant enough to cover that single commit.
    setup_grant_storage_token(make_contract_address(0),
                              replay_cache_cost_charged(1));
  }
};

// Regression for #2847: auth-token expiry must be validated against the
// executing block height (parent tip + 1), not the last-committed parent tip.
// A transaction executing in block H+1 must reject a token whose expiry is H
// (already expired for that block), and observe GetBlockHeight() == H+1.
TEST_F(SyscallAuthTokenExpiryTest, ExpiryAtParentTipRejectedForExecutingBlock)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  // Executing height is the block being built (parent tip + 1). The harness
  // knows both independently of the (previously buggy) GetBlockHeight syscall.
  const BlockHeight executing_height = get_block_number();
  const auto parent_tip = BlockHeight{executing_height - 1};

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // GetBlockHeight reports the executing height, not the parent tip.
        BlockHeight observed_height{};
        auto h_res =
            co_await ctx.syscall<SyscallNo::kGetBlockHeight>(observed_height);
        EXPECT_TRUE(h_res.has_value());
        EXPECT_EQ(h_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(observed_height, executing_height);

        auto try_mint =
            [&](BlockHeight expiry,
                uint32_t &id) -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          AuthTokenNode node{.consumer_address = kAddr,
                             .payload = x23::serialize(1U),
                             .child_nodes = {}};
          auto serialized = x23::serialize(node);
          co_return co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              expiry,
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              id);
        };

        // expiry == parent tip == executing_height - 1: already expired for the
        // executing block, so mint must be rejected.
        {
          uint32_t id = 0;
          auto r = co_await try_mint(parent_tip, id);
          EXPECT_TRUE(r.has_value());
          EXPECT_EQ(r.value(),
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        }

        // expiry == executing height: still valid, mint and consume succeed.
        {
          uint32_t id = 0;
          auto r = co_await try_mint(executing_height, id);
          EXPECT_TRUE(r.has_value());
          EXPECT_EQ(r.value(), SyscallErrorCode::kSyscallErrorCodeNone);

          std::array<std::byte, 256> buf{};
          RuntimeMemoryLength w = 0;
          uint32_t s = 0, c = 0;
          ContractAddress m{};
          auto cr = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
              id, std::span<std::byte>{buf}, m, w, s, c);
          EXPECT_TRUE(cr.has_value());
          EXPECT_EQ(cr.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          EXPECT_EQ(w, x23::serialize(1U).size());
        }

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenExpiryTest, ConsumeAuthTokenExpiryBoundary)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        BlockHeight current_height{};
        auto h_res =
            co_await ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
        EXPECT_TRUE(h_res.has_value());
        EXPECT_EQ(h_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_GT(current_height, BlockHeight{0});

        auto make_and_mint = [&](BlockHeight expiry) -> x23::Async<uint32_t> {
          AuthTokenNode node{.consumer_address = kAddr,
                             .payload = x23::serialize(1U),
                             .child_nodes = {}};
          auto serialized = x23::serialize(node);
          uint32_t id = 0;
          auto r = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              expiry,
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              id);
          EXPECT_TRUE(r.has_value());
          EXPECT_EQ(r.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          co_return id;
        };

        // expiry == block height: token is NOT expired (< is false)
        {
          uint32_t id = co_await make_and_mint(current_height);
          std::array<std::byte, 256> buf{};
          RuntimeMemoryLength w = 0;
          uint32_t s = 0, c = 0;
          ContractAddress m{};
          auto r = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
              id, std::span<std::byte>{buf}, m, w, s, c);
          EXPECT_TRUE(r.has_value());
          EXPECT_EQ(r.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          EXPECT_EQ(w, x23::serialize(1U).size());
        }

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenExpiryTest, MintAuthTokenExpiryBoundary)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        BlockHeight current_height{};
        auto h_res =
            co_await ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
        EXPECT_TRUE(h_res.has_value());
        EXPECT_EQ(h_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_GT(current_height, BlockHeight{0});

        auto try_mint = [&](BlockHeight expiry)
            -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          AuthTokenNode node{.consumer_address = kAddr,
                             .payload = x23::serialize(1U),
                             .child_nodes = {}};
          auto serialized = x23::serialize(node);
          uint32_t id = 0;
          co_return co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              expiry,
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              id);
        };

        // past expiry: rejected
        auto past = co_await try_mint(BlockHeight{current_height - 1});
        EXPECT_TRUE(past.has_value());
        EXPECT_EQ(past.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        // current height: accepted (< is false)
        auto present = co_await try_mint(current_height);
        EXPECT_TRUE(present.has_value());
        EXPECT_EQ(present.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // future height: accepted
        auto future = co_await try_mint(BlockHeight{current_height + 1});
        EXPECT_TRUE(future.has_value());
        EXPECT_EQ(future.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}
// MintAuthToken bytes that fail x23 deserialization (not a serialized
// AuthTokenNode) are rejected as kInvalidSyscallInput -- the XdrException
// catch arm. Every other mint in the suite serializes a valid node, so this
// is the only pin on the malformed-bytes arm. A valid mint afterwards
// proves the handler survives the rejected call.
TEST_F(SyscallAuthTokenExpiryTest, MintAuthTokenRejectsUndeserializableBytes)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        BlockHeight current_height{};
        auto h_res =
            co_await ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
        EXPECT_TRUE(h_res.has_value());
        EXPECT_EQ(h_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        constexpr std::array<std::byte, 4> kGarbage = {std::byte{0xde},
                                                       std::byte{0xad},
                                                       std::byte{0xbe},
                                                       std::byte{0xef}};
        uint32_t id = 0xDEAD'BEEF;
        auto res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{current_height + 10},
            std::span<const std::byte>(kGarbage), id);
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        EXPECT_EQ(id, 0xDEAD'BEEF) << "output must be untouched on reject";

        AuthTokenNode node{.consumer_address = kAddr,
                           .payload = x23::serialize(1U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(node);
        auto ok = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{current_height + 10},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            id);
        EXPECT_TRUE(ok.has_value());
        EXPECT_EQ(ok.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Child consumes a token, gets unwrapped child tokens, returns them to
// the parent via the Return syscall.  Parent then consumes the returned token.
//
// This exercises the full round-trip: mint → forward → consume → Return
// with returned_auth_token_ids → parent owns the returned tokens.
//
// EXPECTED TO FAIL until set_child_forwarded_auth_tokens is called on the
// parent frame (not the child frame) in set_return_buffer.
TEST_F(SyscallAuthTokenTest, ReturnAuthTokensToParent)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // Child: receives a forwarded token via calldata, consumes it,
  // then returns the unwrapped child tokens to the parent via Return.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        // Consume the token → get payload + unwrapped child tokens
        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        uint32_t child_start = 0, child_count = 0;
        ContractAddress minter{};
        auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, child_start, child_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(minter, kCallerAddress);
        EXPECT_EQ(child_count, 1U);

        // Build the returned_auth_token_ids span (the unwrapped child)
        std::array<uint32_t, 1> returned_ids{child_start};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{returned_ids});

        // Call Return via ctx.syscall to go through the full path
        std::array<std::byte, 4> ret_buf{};
        std::memcpy(ret_buf.data(), "OK!!", 4);
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{ret_buf}, false, auth_bytes);
        // Return always yields HostFnError::Return (non-recoverable)
        EXPECT_FALSE(ret.has_value());

        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints a nested token (root → child), invokes child forwarding
  // the root, then tries to consume the returned child token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // Build a two-level token tree:
        //   root  (consumer=child, payload=42)
        //     └─ leaf (consumer=caller, payload=99)
        AuthTokenNode leaf{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        AuthTokenNode root{.consumer_address = kChildAddress,
                           .payload = x23::serialize(42U),
                           .child_nodes = {leaf}};
        auto serialized = x23::serialize(root);
        uint32_t root_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            root_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Invoke child, forwarding the root token
        Calldata calldata;
        calldata.resize_uninitialized(sizeof(root_id));
        std::memcpy(calldata.data(), &root_id, sizeof(root_id));

        std::array<uint32_t, 1> forwarded{root_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        uint32_t leaf_id = root_id + 1;

        // Invoke reports how many tokens the child returned.
        EXPECT_EQ(outputs.returned_auth_token_count, 1U);

        // GetReturnedBuffer reports the ids they took in this frame.
        std::array<std::byte, 256> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        uint32_t returned_start = 0;
        uint32_t returned_count = 0;
        auto get_res = co_await ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            returned_start, returned_count);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(return_written_len, 4U);
        EXPECT_EQ(returned_start, leaf_id);
        EXPECT_EQ(returned_count, 1U);

        // The caller should own the returned token and be able to
        // consume it.
        std::array<std::byte, 256> leaf_payload{};
        RuntimeMemoryLength leaf_payload_len = 0;
        uint32_t unused_start = 0, unused_count = 0;
        ContractAddress leaf_minter{};
        auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            leaf_id, std::span<std::byte>{leaf_payload}, leaf_minter,
            leaf_payload_len, unused_start, unused_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(leaf_minter, kCallerAddress);

        if (consume_res.has_value() &&
            consume_res.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
          auto expected_payload = x23::serialize(99U);
          EXPECT_EQ(leaf_payload_len, expected_payload.size());
          EXPECT_EQ(std::memcmp(leaf_payload.data(), expected_payload.data(),
                                expected_payload.size()),
                    0);
        }

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReturnRejectsDuplicateAuthTokenIds)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode node{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 2> duplicate_ids{token_id, token_id};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{duplicate_ids});
        std::array<std::byte, 4> ret_buf{};
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{ret_buf}, false, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);

        co_return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Fault);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Return syscall with auth token IDs that the child doesn't own must
// cause a deterministic exit (fault), not silently succeed.
TEST_F(SyscallAuthTokenTest, ReturnUnownedAuthTokensCausesExit)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // Claim to co_return token ID 9999, which we don't own.
        std::array<uint32_t, 1> bogus_ids{9999};
        auto auth_bytes = std::as_bytes(std::span<const uint32_t>{bogus_ids});

        std::array<std::byte, 4> buf{};
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{buf}, false, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);

        co_return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Fault);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Regression test for #1702: a child mints a token then faults; the parent
// must not be able to forward the child-minted token ID to another contract.
TEST_F(SyscallAuthTokenTest, ForwardUnownedAuthTokenAfterChildRevertIsRejected)
{
  constexpr ContractAddress kAttackerAddress = make_contract_address(0);
  constexpr ContractAddress kMinterAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op — should never execute.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        ADD_FAILURE() << "Consumer should never be invoked";
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Minter: mints a token targeting the consumer, then faults.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_minter;
  fns_minter.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kConsumerAddress,
                                .payload = x23::serialize(0xDEADU),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Write the minted token ID back to the caller via co_return buffer
        // before faulting, so the attacker knows which ID to forge.
        std::array<std::byte, sizeof(uint32_t)> ret_buf{};
        std::memcpy(ret_buf.data(), &token_id, sizeof(token_id));
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{ret_buf}, true,
            std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());

        co_return InvokeStatus<void>{};
      });
  auto minter_contract =
      std::make_unique<NativeContract>(std::move(fns_minter));

  // Attacker: invokes minter via try-invoke, then tries to forward the
  // minter's token ID to the consumer.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_attacker;
  fns_attacker.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // Step 1: invoke minter with TryInvoke — it will mint and fault.
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::TryInvoke),
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kMinterAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Abort);

        // Read the token ID the minter wrote into its co_return buffer.
        std::array<std::byte, 256> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        uint32_t returned_start = 0;
        uint32_t returned_count = 0;
        auto get_res = co_await ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            returned_start, returned_count);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        if (return_written_len != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in co_return buffer";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint32_t stolen_token_id = 0;
        std::memcpy(&stolen_token_id, return_buf.data(), sizeof(uint32_t));

        // Step 2: try to forward the stolen token ID to the consumer.
        // This must be rejected because the attacker doesn't own it.
        std::array<uint32_t, 1> forged_ids{stolen_token_id};
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto forge_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forged_ids}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(forge_res.has_value());
        EXPECT_EQ(forge_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto attacker_contract =
      std::make_unique<NativeContract>(std::move(fns_attacker));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(attacker_contract));
  contract_map.emplace(1, std::move(minter_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto attacker_xdr = make_native_contract(0);
  write_contract(attacker_xdr);
  write_deployment(kAttackerAddress, attacker_xdr);
  auto minter_xdr = make_native_contract(1);
  write_contract(minter_xdr);
  write_deployment(kMinterAddress, minter_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAttackerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Regression test for #1702 comment 1: forwarding a completely fabricated
// token ID (never minted) must be rejected, not crash via CHECK_PTR.
TEST_F(SyscallAuthTokenTest, ForwardFabricatedAuthTokenIdIsRejected)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kCalleeAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_callee;
  fns_callee.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        ADD_FAILURE() << "Callee should never be invoked";
        co_return InvokeStatus<void>{};
      });
  auto callee_contract =
      std::make_unique<NativeContract>(std::move(fns_callee));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        constexpr uint32_t kFabricatedId = 123456;
        std::array<uint32_t, 1> forged_ids{kFabricatedId};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kCalleeAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forged_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(callee_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto callee_xdr = make_native_contract(1);
  write_contract(callee_xdr);
  write_deployment(kCalleeAddress, callee_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Regression test for #1702 comment 2: after a child consumes a token, the
// parent must not be able to re-forward that consumed token ID.
TEST_F(SyscallAuthTokenTest, ReplayConsumedAuthTokenIsRejected)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        uint32_t child_start = 0, child_count = 0;
        ContractAddress minter{};
        auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, child_start, child_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root{.consumer_address = kChildAddress,
                           .payload = x23::serialize(42U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        Calldata calldata;
        calldata.resize_uninitialized(sizeof(token_id));
        std::memcpy(calldata.data(), &token_id, sizeof(token_id));

        // First invoke: child consumes the token successfully.
        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto first_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(first_res.has_value());
        EXPECT_EQ(first_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Second invoke: parent no longer owns the token.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto second_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs2, CopyableInvokeOutputs{});
        EXPECT_TRUE(second_res.has_value());
        EXPECT_EQ(second_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Regression test for #1711: a middle contract learns a token ID from calldata
// but does not own it; forwarding it to the consumer must be rejected.
TEST_F(SyscallAuthTokenTest, MiddleContractCannotForwardUnownedToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kMiddleAddress = make_contract_address(1);
  constexpr ContractAddress kLeafAddress = make_contract_address(2);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_leaf;
  fns_leaf.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        ADD_FAILURE() << "Leaf should never be invoked";
        co_return InvokeStatus<void>{};
      });
  auto leaf_contract = std::make_unique<NativeContract>(std::move(fns_leaf));

  // Middle: receives the token ID in calldata (not forwarded), tries to
  // forward it to the leaf.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_middle;
  fns_middle.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<uint32_t, 1> stolen_ids{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kLeafAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{stolen_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        co_return InvokeStatus<void>{};
      });
  auto middle_contract =
      std::make_unique<NativeContract>(std::move(fns_middle));

  // Caller: mints a token targeting the leaf, then invokes the middle
  // WITHOUT forwarding the token — only passes the ID in calldata.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root{.consumer_address = kLeafAddress,
                           .payload = x23::serialize(77U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Pass the token ID in calldata, but do NOT forward it.
        Calldata calldata;
        calldata.resize_uninitialized(sizeof(token_id));
        std::memcpy(calldata.data(), &token_id, sizeof(token_id));

        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kMiddleAddress, calldata, std::span<uint32_t const>{}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(middle_contract));
  contract_map.emplace(2, std::move(leaf_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto middle_xdr = make_native_contract(1);
  write_contract(middle_xdr);
  write_deployment(kMiddleAddress, middle_xdr);
  auto leaf_xdr = make_native_contract(2);
  write_contract(leaf_xdr);
  write_deployment(kLeafAddress, leaf_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

namespace {

std::span<const std::byte>
as_byte_span(x23::Buf const &buf)
{
  return std::as_bytes(std::span<const uint8_t>{buf.data(), buf.size()});
}

// A single root-to-leaf chain of `depth` nodes, each carrying its level (0 at
// the root) as payload.
AuthTokenNode
make_auth_token_chain(ContractAddress const &consumer, uint32_t depth)
{
  AuthTokenNode node{.consumer_address = consumer,
                     .payload = x23::serialize(depth - 1),
                     .child_nodes = {}};
  for (uint32_t level = depth - 1; level > 0; --level) {
    node = AuthTokenNode{.consumer_address = consumer,
                         .payload = x23::serialize(level - 1),
                         .child_nodes = {node}};
  }
  return node;
}

// A full kAuthTokenMaxChildNodes-way tree of `depth` levels, payload = level.
AuthTokenNode
make_full_auth_token_tree(ContractAddress const &consumer, uint32_t depth)
{
  AuthTokenNode node{.consumer_address = consumer,
                     .payload = x23::serialize(depth - 1),
                     .child_nodes = {}};
  for (uint32_t level = depth - 1; level > 0; --level) {
    AuthTokenNode parent{.consumer_address = consumer,
                         .payload = x23::serialize(level - 1),
                         .child_nodes = {}};
    for (uint32_t i = 0; i < uint32_t{kAuthTokenMaxChildNodes}; ++i) {
      parent.child_nodes.push_back(node);
    }
    node = std::move(parent);
  }
  return node;
}

constexpr uint32_t
full_tree_node_count(uint32_t width, uint32_t depth)
{
  uint32_t count = 0;
  uint32_t level_nodes = 1;
  for (uint32_t level = 0; level < depth; ++level) {
    count += level_nodes;
    level_nodes *= width;
  }
  return count;
}

// Decodes as the MintAuthToken syscall does; nullopt iff refused for depth.
std::optional<AuthTokenNode>
decode_under_token_depth_bound(std::span<const uint8_t> bytes)
{
  AuthTokenNode out;
  try {
    x23::deserialize(&out, x23::BufView{bytes}, size_t{kAuthTokenMaxDepth});
  } catch (x23::XdrBadInput const &e) {
    EXPECT_STREQ(
        e.what(),
        std::format("x23::Deserializer: exceeded recursion limit of {}",
                    kAuthTokenMaxDepth)
            .c_str());
    return std::nullopt;
  }
  return out;
}

} // namespace

// The decoder's budget is the mint path's only depth check.
TEST(AuthTokenDepthBoundTest, ChainDecodesIffWithinMaxDepth)
{
  constexpr ContractAddress kAddr =
      NativeContractTestFramework::make_contract_address(0);
  for (uint32_t depth = 1; depth <= uint32_t{kAuthTokenMaxDepth} + 2; ++depth) {
    SCOPED_TRACE(depth);
    auto chain = make_auth_token_chain(kAddr, depth);
    bool const within = depth <= uint32_t{kAuthTokenMaxDepth};
    auto decoded = decode_under_token_depth_bound(x23::serialize(chain));
    ASSERT_EQ(decoded.has_value(), within);
    if (decoded.has_value()) {
      EXPECT_EQ(*decoded, chain);
    }
  }
}

// Width never consumes budget; one node under the last leaf still exceeds it.
TEST(AuthTokenDepthBoundTest, FullWidthTreeDecodesUnlessABranchIsTooDeep)
{
  constexpr ContractAddress kAddr =
      NativeContractTestFramework::make_contract_address(0);
  auto tree = make_full_auth_token_tree(kAddr, uint32_t{kAuthTokenMaxDepth});
  auto decoded = decode_under_token_depth_bound(x23::serialize(tree));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, tree);

  // Hang one more node under the last leaf the decoder reaches.
  AuthTokenNode *last = &tree;
  while (!last->child_nodes.empty()) {
    last = &last->child_nodes.back();
  }
  last->child_nodes.push_back(
      AuthTokenNode{.consumer_address = kAddr,
                    .payload = x23::serialize(uint32_t{kAuthTokenMaxDepth}),
                    .child_nodes = {}});
  EXPECT_FALSE(decode_under_token_depth_bound(x23::serialize(tree)));
}

TEST_F(SyscallAuthTokenTest, MintAuthTokenRejectsOverDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto leaf = make_auth_token_chain(kAddr, 1);
        auto serialized_leaf = x23::serialize(leaf);
        uint32_t first_id = 0;
        auto first_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000}, as_byte_span(serialized_leaf), first_id);
        EXPECT_TRUE(first_res.has_value());
        EXPECT_EQ(first_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        auto over_depth =
            make_auth_token_chain(kAddr, uint32_t{kAuthTokenMaxDepth} + 1);
        auto serialized = x23::serialize(over_depth);
        uint32_t token_id = 0xDEAD'BEEF;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000}, as_byte_span(serialized), token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        EXPECT_EQ(token_id, 0xDEAD'BEEF)
            << "output must be untouched on reject";

        // The decoder refuses the token before any id is allocated, so the
        // next mint gets the id right after the one minted before it.
        uint32_t second_id = 0;
        auto second_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000}, as_byte_span(serialized_leaf), second_id);
        EXPECT_TRUE(second_res.has_value());
        EXPECT_EQ(second_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(second_id, first_id + 1);

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Each mint retains its decoded tree for the base crossing charge, so a
// transaction may attempt at most max_auth_tokens mints (#4653). A failed
// decode counts too.
TEST_F(SyscallAuthTokenTest, MintAuthTokenStopsAtMaxAuthTokens)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto over_depth = x23::serialize(
            make_auth_token_chain(kAddr, uint32_t{kAuthTokenMaxDepth} + 1));
        uint32_t failed_id = 0;
        auto failed_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000}, as_byte_span(over_depth), failed_id);
        EXPECT_TRUE(failed_res.has_value());
        EXPECT_EQ(failed_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        auto serialized = x23::serialize(make_auth_token_chain(kAddr, 1));
        for (uint32_t i = 0; i + 1 < kDefaultMaxAuthTokens; ++i) {
          uint32_t token_id = 0;
          auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              BlockHeight{1000}, as_byte_span(serialized), token_id);
          EXPECT_TRUE(mint_res.has_value());
          EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          EXPECT_EQ(token_id, i);
        }

        uint32_t token_id = 0xDEAD'BEEF;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000}, as_byte_span(serialized), token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        EXPECT_EQ(token_id, 0xDEAD'BEEF)
            << "output must be untouched on reject";

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, MintAuthTokenAcceptsMaxDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode node{.consumer_address = kAddr,
                           .payload =
                               x23::serialize(uint32_t{kAuthTokenMaxDepth - 1}),
                           .child_nodes = {}};
        for (uint32_t level = kAuthTokenMaxDepth - 1; level > 0; --level) {
          node = AuthTokenNode{.consumer_address = kAddr,
                               .payload = x23::serialize(level - uint32_t{1}),
                               .child_nodes = {node}};
        }

        auto serialized = x23::serialize(node);
        uint32_t root_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            root_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        uint32_t token_id = root_id;
        for (uint32_t level = 0; level < uint32_t{kAuthTokenMaxDepth};
             ++level) {
          std::array<std::byte, 256> payload_buf{};
          RuntimeMemoryLength payload_written_len = 0;
          uint32_t child_start = 0, child_count = 0;
          ContractAddress loop_minter{};
          auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
              token_id, std::span<std::byte>{payload_buf}, loop_minter,
              payload_written_len, child_start, child_count);
          EXPECT_TRUE(consume_res.has_value());
          EXPECT_EQ(consume_res.value(),
                    SyscallErrorCode::kSyscallErrorCodeNone);
          EXPECT_EQ(loop_minter, kAddr);

          auto expected_payload = x23::serialize(level);
          EXPECT_EQ(payload_written_len, expected_payload.size());
          EXPECT_EQ(std::memcmp(payload_buf.data(), expected_payload.data(),
                                expected_payload.size()),
                    0);

          if (level < uint32_t{kAuthTokenMaxDepth} - 1) {
            EXPECT_EQ(child_count, 1U);
            token_id = child_start;
          }
          else {
            EXPECT_EQ(child_count, 0U);
          }
        }

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, MintAuthTokenRejectsOverDepthBranchingTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode too_deep{.consumer_address = kAddr,
                               .payload = x23::serialize(uint32_t{5}),
                               .child_nodes = {}};
        AuthTokenNode great_grandchild{.consumer_address = kAddr,
                                       .payload = x23::serialize(uint32_t{4}),
                                       .child_nodes = {too_deep}};
        AuthTokenNode grandchild{.consumer_address = kAddr,
                                 .payload = x23::serialize(uint32_t{3}),
                                 .child_nodes = {great_grandchild}};
        AuthTokenNode child1{.consumer_address = kAddr,
                             .payload = x23::serialize(uint32_t{2}),
                             .child_nodes = {grandchild}};
        AuthTokenNode child0{.consumer_address = kAddr,
                             .payload = x23::serialize(uint32_t{1}),
                             .child_nodes = {}};
        AuthTokenNode root{.consumer_address = kAddr,
                           .payload = x23::serialize(uint32_t{0}),
                           .child_nodes = {child0, child1}};

        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// The widest and deepest token the bounds admit: mint and consume every node.
TEST_F(SyscallAuthTokenTest, MintAuthTokenAcceptsFullWidthMaxDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);
  constexpr uint32_t kNodeCount =
      full_tree_node_count(uint32_t{kAuthTokenMaxChildNodes},
                           uint32_t{kAuthTokenMaxDepth});

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto tree =
            make_full_auth_token_tree(kAddr, uint32_t{kAuthTokenMaxDepth});
        auto serialized = x23::serialize(tree);
        uint32_t root_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000}, as_byte_span(serialized), root_id);
        EXPECT_TRUE(mint_res.has_value());
        if (!mint_res.has_value() ||
            mint_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "MintAuthToken refused the maximal token";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // Consume every node reachable from the root, tracking each id's
        // level so its payload and child count can be checked.
        std::vector<std::pair<uint32_t, uint32_t>> pending;
        pending.emplace_back(root_id, 0U);
        uint32_t consumed = 0;
        while (!pending.empty()) {
          auto [token_id, level] = pending.back();
          pending.pop_back();

          std::array<std::byte, kAuthTokenMaxLen> payload_buf{};
          RuntimeMemoryLength payload_written_len = 0;
          uint32_t child_start = 0, child_count = 0;
          ContractAddress minter{};
          auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
              token_id, std::span<std::byte>{payload_buf}, minter,
              payload_written_len, child_start, child_count);
          EXPECT_TRUE(consume_res.has_value());
          if (!consume_res.has_value() ||
              consume_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "ConsumeAuthToken failed at level " << level;
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          EXPECT_EQ(minter, kAddr);

          auto expected_payload = x23::serialize(level);
          EXPECT_EQ(payload_written_len, expected_payload.size());
          EXPECT_EQ(std::memcmp(payload_buf.data(), expected_payload.data(),
                                expected_payload.size()),
                    0);

          bool const is_leaf = level + 1 == uint32_t{kAuthTokenMaxDepth};
          EXPECT_EQ(child_count,
                    is_leaf ? 0U : uint32_t{kAuthTokenMaxChildNodes});
          for (uint32_t i = 0; i < child_count; ++i) {
            pending.emplace_back(child_start + i, level + 1);
          }
          ++consumed;
        }
        EXPECT_EQ(consumed, kNodeCount);

        co_return InvokeStatus<void>{};
      });
  auto contract = std::make_unique<NativeContract>(std::move(fns_contract));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_xdr = make_native_contract(0);
  write_contract(contract_xdr);
  write_deployment(kAddr, contract_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kAddr,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyInvokeDropsUnreturnedForwardedAuthTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: should never be reached.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        ADD_FAILURE() << "Consumer should never be invoked";
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: receives the token but does not consume or return it.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints a token, forwards it to the read-only child, then tries
  // to forward the same token to the consumer (should fail).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // Mint a token targeting the child.
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Forward the token to the child via a read-only invoke.
        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::ReadOnly),
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // The token was dropped by the read-only child. Attempting to
        // forward it again must fail — the caller no longer owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildReturnsForwardedAuthToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op, just proves the caller still owns the token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: explicitly returns the forwarded token, its id 0, via
  // kReturn.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::array<uint32_t, 1> returned_ids{0};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{returned_ids});
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, false, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints token, forwards to read-only child that returns it under the
  // caller's next id, then forwards it under that id to consumer (should
  // succeed).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::ReadOnly),
        };
        CopyableInvokeOutputs outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.returned_auth_token_count, 1U);

        // The returned token takes the caller's next id.
        std::array<std::byte, 256> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        uint32_t returned_start = 0;
        uint32_t returned_count = 0;
        auto get_res = co_await ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            returned_start, returned_count);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(returned_start, token_id + 1);
        EXPECT_EQ(returned_count, 1U);
        std::array<uint32_t, 1> returned{returned_start};

        // Token was explicitly returned: caller owns it under the new id.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{returned}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildCannotConsumeForwardedToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // Read-only child: tries to consume the forwarded token, should be rejected.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        uint32_t child_start = 0, child_count = 0;
        ContractAddress minter{};
        auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, child_start, child_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        Calldata calldata;
        calldata.resize_uninitialized(sizeof(token_id));
        std::memcpy(calldata.data(), &token_id, sizeof(token_id));

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::ReadOnly),
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildCannotMintToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // Read-only child: tries to mint a token, should be rejected.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode node{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::ReadOnly),
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildFaultRestoresForwardedTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op, proves the caller still owns the token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: faults deterministically.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints token, forwards to read-only child via TryInvoke+ReadOnly,
  // child faults, caller verifies token is restored.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::TryInvoke) |
                     std::to_underlying(InvokeFlag::ReadOnly),
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Fault);

        // Token was restored after the fault — caller still owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildManualRevertRestoresForwardedTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op, proves the caller still owns the token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: manually reverts via kReturn with revert_frame=true.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, true, std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints token, forwards to read-only child that manually reverts,
  // caller verifies token is restored.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::ReadOnly),
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Abort);

        // Token was restored after the manual revert — caller still owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// Return(revert_frame=true) with auth token IDs must trap, even when
// the child legitimately owns the token (minted by the parent).
TEST_F(SyscallAuthTokenTest, ReturnRevertWithMintedAuthTokenCausesExit)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<uint32_t, 1> returned_ids{token_id};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{returned_ids});
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, true, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root{.consumer_address = kChildAddress,
                           .payload = x23::serialize(0xBEEFU),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        Calldata calldata;
        calldata.resize_uninitialized(sizeof(token_id));
        std::memcpy(calldata.data(), &token_id, sizeof(token_id));

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Fault);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// A token minted inside a manually-reverted child frame must not be usable
// by the parent.  The revert is a no-op, so the minted token ceases to exist.
TEST_F(SyscallAuthTokenTest, MintedTokenInRevertedChildNotOwnedByParent)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kLeafAddress = make_contract_address(2);

  uint32_t child_minted_token_id = UINT32_MAX;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(0xCAFEU),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        child_minted_token_id = token_id;

        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, true, std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_leaf;
  fns_leaf.emplace(
      0, [](NativeSyscallContext &) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto leaf_contract = std::make_unique<NativeContract>(std::move(fns_leaf));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, invoke_outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(invoke_outputs.frame_status,
                  InvocationFrameStatusCode::Abort);

        EXPECT_NE(child_minted_token_id, UINT32_MAX);

        std::array<uint32_t, 1> forwarded{child_minted_token_id};
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto forward_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kLeafAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(forward_res.has_value());
        EXPECT_EQ(forward_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(leaf_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto leaf_xdr = make_native_contract(2);
  write_contract(leaf_xdr);
  write_deployment(kLeafAddress, leaf_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// #2543: auth token ids are frame-local, so a manually reverted child's consume
// leaves no mark on later id allocation.
TEST_F(SyscallAuthTokenTest, RevertedConsumeLeavesNoTraceOnAuthTokenIds)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  struct ConsumeRange {
    uint32_t start = UINT32_MAX;
    uint32_t count = UINT32_MAX;
  };

  struct ScenarioResult {
    uint32_t root_id = UINT32_MAX;
    ConsumeRange reverted_consume;
    uint32_t intervening_token_id = UINT32_MAX;
    ConsumeRange later_consume;
    bool did_reverted_consume = false;
  };

  ScenarioResult polluted;
  ScenarioResult control;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint8_t)) {
          ADD_FAILURE() << "Expected the revert flag in calldata";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint8_t revert_flag = calldata[0];
        // The forwarded token is the child's id 0.
        uint32_t token_id = 0;

        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        ConsumeRange range;
        ContractAddress minter{};
        auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, range.start, range.count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(minter, kCallerAddress);
        EXPECT_EQ(range.count, 1U);

        std::array<uint32_t, 2> range_words{range.start, range.count};
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::as_bytes(std::span<const uint32_t>{range_words}),
            revert_flag != 0, std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Return);
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  auto make_caller =
      [&](bool do_reverted_consume,
          ScenarioResult &scenario) -> NativeContract::function_t {
    return [&, do_reverted_consume](
               NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
      auto mint_token = [&](AuthTokenNode const &root_node,
                            uint32_t &token_id) -> x23::Async<bool> {
        auto serialized = x23::serialize(root_node);
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        co_return mint_res.has_value() &&
            mint_res.value() == SyscallErrorCode::kSyscallErrorCodeNone;
      };

      auto read_last_child_range =
          [&](ConsumeRange &range) -> x23::Async<bool> {
        std::array<std::byte, sizeof(uint32_t) * 2> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        uint32_t returned_start = 0;
        uint32_t returned_count = 0;
        auto get_res = co_await ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            returned_start, returned_count);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(static_cast<size_t>(return_written_len), return_buf.size());
        EXPECT_EQ(returned_count, 0U);
        if (!get_res.has_value() ||
            get_res.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
            static_cast<size_t>(return_written_len) != return_buf.size()) {
          co_return false;
        }

        std::array<uint32_t, 2> range_words{};
        std::memcpy(range_words.data(), return_buf.data(), return_buf.size());
        range.start = range_words[0];
        range.count = range_words[1];
        co_return true;
      };

      auto invoke_child = [&](uint32_t token_id, bool revert_frame,
                              ConsumeRange &range) -> x23::Async<bool> {
        Calldata calldata;
        calldata.push_back(revert_frame ? 1 : 0);

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.frame_status,
                  revert_frame ? InvocationFrameStatusCode::Abort
                               : InvocationFrameStatusCode::Return);
        EXPECT_EQ(static_cast<size_t>(outputs.return_buffer_len),
                  sizeof(uint32_t) * 2);
        EXPECT_EQ(outputs.returned_auth_token_count, 0U);
        if (!invoke_res.has_value() ||
            invoke_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return false;
        }
        co_return co_await read_last_child_range(range);
      };

      AuthTokenNode child{.consumer_address = kCallerAddress,
                          .payload = x23::serialize(99U),
                          .child_nodes = {}};
      AuthTokenNode root{.consumer_address = kChildAddress,
                         .payload = x23::serialize(42U),
                         .child_nodes = {child}};
      if (!co_await mint_token(root, scenario.root_id)) {
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }

      if (do_reverted_consume) {
        scenario.did_reverted_consume = true;
        if (!co_await invoke_child(scenario.root_id, true,
                                   scenario.reverted_consume)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
      }

      AuthTokenNode unrelated{.consumer_address = kCallerAddress,
                              .payload = x23::serialize(123U),
                              .child_nodes = {}};
      if (!co_await mint_token(unrelated, scenario.intervening_token_id)) {
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }

      // The caller's ids moved on by its own mint alone.
      EXPECT_EQ(scenario.intervening_token_id, scenario.root_id + 1);

      if (!co_await invoke_child(scenario.root_id, false,
                                 scenario.later_consume)) {
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
      EXPECT_EQ(scenario.later_consume.count, 1U);

      co_return InvokeStatus<void>{};
    };
  };

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(0, make_caller(true, polluted));
  fns_caller.emplace(1, make_caller(false, control));
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  setup_grant_storage_token(kCallerAddress, replay_cache_cost_charged(2));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto polluted_handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              Invocation{
                  .invoked_address = kCallerAddress,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = kDefaultMaxTxGas,
                  .calldata = {},
              },
          .uniquifier = make_new_uniquifier(),
      },
      *runtime_context);
  auto polluted_res = polluted_handler.root_invoke()();
  ASSERT_TRUE(polluted_res.is_root_success());

  auto control_handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              Invocation{
                  .invoked_address = kCallerAddress,
                  .entrypoint = entrypoint_from_method_id(1),
                  .gas_limit = kDefaultMaxTxGas,
                  .calldata = {},
              },
          .uniquifier = make_new_uniquifier(),
      },
      *runtime_context);
  auto control_res = control_handler.root_invoke()();
  ASSERT_TRUE(control_res.is_root_success());

  ASSERT_TRUE(polluted.did_reverted_consume);
  EXPECT_EQ(polluted.reverted_consume.count, 1U);
  EXPECT_EQ(polluted.later_consume.count, 1U);
  EXPECT_EQ(control.later_consume.count, 1U);

  // Each consume numbers the children in its own frame, after the forwarded
  // token.
  EXPECT_EQ(polluted.reverted_consume.start, 1U);
  EXPECT_EQ(polluted.later_consume.start, 1U);
  EXPECT_EQ(control.later_consume.start, 1U);
  // The reverted consume left the caller's ids as the control path has them.
  EXPECT_EQ(polluted.root_id, control.root_id);
  EXPECT_EQ(polluted.intervening_token_id, control.intervening_token_id);
}

// A child frame that runs out of gas cannot hand a freshly minted auth token
// back to its parent.
//
// This is the frame-level half of the #1759 defence. syscall_entry_outer
// rewrites a zero-cost syscall to OutOfGas once the frame is exhausted, but
// the inner syscall has already run by then -- MintAuthToken registers the
// token in the active frame before the rewrite happens. What makes that safe
// is that OutOfGas reverts the frame, and commit_failed_child_frame drops the
// child's returned-token set instead of merging it into the parent's owned
// set. Both halves have to hold: if either one regresses, the parent ends up
// owning a capability that an out-of-gas child created.
TEST_F(SyscallAuthTokenTest, OutOfGasChildCannotReturnMintedAuthToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // The token ids a child could plausibly have been handed. Ids are allocated
  // sequentially from zero within a transaction, and nothing else in this test
  // mints, so a leaked token would land at the front of the range.
  constexpr std::array<uint32_t, 4> kCandidateTokenIds{0, 1, 2, 3};

  struct ChildObservations {
    bool ran = false;
    bool mint_succeeded = false;
    bool return_succeeded = false;
  };
  ChildObservations child_obs;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        child_obs.ran = true;

        AuthTokenNode root_node{.consumer_address = kCallerAddress,
                                .payload = x23::serialize(99U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        child_obs.mint_succeeded =
            mint_res.has_value() &&
            mint_res.value() == SyscallErrorCode::kSyscallErrorCodeNone;

        // Try to hand the token back regardless of what mint reported -- the
        // exploit shape is mint-then-Return, and Return is zero-cost too.
        std::array<uint32_t, 1> returned_ids{token_id};
        std::array<std::byte, 4> ret_buf{};
        auto ret_res = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{ret_buf}, false,
            std::as_bytes(std::span<const uint32_t>{returned_ids}));
        // Return reports success through the error channel, as HostFnError.
        child_obs.return_succeeded =
            !ret_res.has_value() &&
            ret_res.error().code() == HostFnErrorCode::Return;

        co_return InvokeError{InvokeErrorCode::OutOfGas};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            // The child frame starts with nothing to spend, so its first
            // syscall is entered with the gas already exhausted -- the state
            // an over-budget guest contract reaches at a call boundary.
            .gas_limit = 0,
            .flags = 0,
        };
        CopyableInvokeOutputs invoke_outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{}, inputs, invoke_outputs);

        // The child either never started or faulted; either way it must not
        // report a successful frame return.
        if (invoke_res.has_value() &&
            invoke_res.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
          EXPECT_EQ(invoke_outputs.frame_status,
                    InvocationFrameStatusCode::Fault);
        }

        // Nothing came back across the frame boundary.
        EXPECT_EQ(invoke_outputs.returned_auth_token_count, 0U);

        // And the parent owns no token the child could have minted.
        for (uint32_t candidate : kCandidateTokenIds) {
          EXPECT_FALSE(ctx.handler().check_auth_tokens_owned(
              std::span<uint32_t const>{&candidate, 1}))
              << "parent owns token id " << candidate
              << " minted by an out-of-gas child";
        }

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  // If the child ran at all, neither zero-cost syscall may have succeeded.
  if (child_obs.ran) {
    EXPECT_FALSE(child_obs.mint_succeeded)
        << "MintAuthToken succeeded in a frame with no gas";
    EXPECT_FALSE(child_obs.return_succeeded)
        << "Return succeeded in a frame with no gas";
  }
}

// Ids are frame-local, so list order determines which token is which; distinct
// payloads catch any reordering.
TEST_F(SyscallAuthTokenTest,
       ForwardedAndReturnedAuthTokenListsKeepContractOrder)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);
  constexpr uint32_t kTokenCount = 3;

  // Consumer: consumes its forwarded tokens, ids 0..2, recording payloads.
  std::vector<AuthTokenPayload> consumed_payloads;
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (uint32_t token_id = 0; token_id < kTokenCount; ++token_id) {
          std::array<std::byte, 256> payload_buf{};
          RuntimeMemoryLength payload_written_len = 0;
          ContractAddress minter{};
          uint32_t unwrapped_start = 0;
          uint32_t unwrapped_count = 0;
          auto res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
              token_id, std::span<std::byte>{payload_buf}, minter,
              payload_written_len, unwrapped_start, unwrapped_count);
          EXPECT_TRUE(res.has_value());
          EXPECT_EQ(res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          auto const *bytes =
              reinterpret_cast<uint8_t const *>(payload_buf.data());
          consumed_payloads.emplace_back(bytes, bytes + payload_written_len);
        }
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Child: sees its forwarded tokens as 0..2 and returns them rotated by one.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        RuntimeMemoryLength calldata_len = 0;
        uint32_t auth_token_count = 0;
        auto len_res =
            co_await ctx.syscall<SyscallNo::kGetCalldataLen>(calldata_len,
                                                             auth_token_count);
        EXPECT_TRUE(len_res.has_value());
        EXPECT_EQ(len_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(auth_token_count, kTokenCount);

        std::array<std::byte, 16> calldata_buf{};
        RuntimeMemoryLength written_len = 0;
        uint32_t calldata_auth_token_count = 0;
        auto get_res = co_await ctx.syscall<SyscallNo::kGetCalldata>(
            std::span<std::byte>{calldata_buf}, written_len,
            calldata_auth_token_count);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(calldata_auth_token_count, kTokenCount);

        std::array<uint32_t, kTokenCount> returned{1, 2, 0};
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, false,
            std::as_bytes(std::span<const uint32_t>{returned}));
        EXPECT_FALSE(ret.has_value());
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller mints payloads 0..2, forwards them rotated, and re-forwards the
  // returned ids to the consumer.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::array<uint32_t, kTokenCount> minted{};
        for (uint32_t i = 0; i < kTokenCount; ++i) {
          AuthTokenNode root_node{.consumer_address = kConsumerAddress,
                                  .payload = x23::serialize(i),
                                  .child_nodes = {}};
          auto serialized = x23::serialize(root_node);
          auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              BlockHeight{1000},
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              minted[i]);
          EXPECT_TRUE(mint_res.has_value());
          EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        }
        EXPECT_EQ(minted, (std::array<uint32_t, kTokenCount>{0, 1, 2}));
        std::array<uint32_t, kTokenCount> forwarded{minted[2], minted[0],
                                                    minted[1]};

        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.frame_status, InvocationFrameStatusCode::Return);
        EXPECT_EQ(outputs.returned_auth_token_count, kTokenCount);

        // The returned tokens take the caller's next ids, 3..5, in the order
        // the child returned them.
        std::array<std::byte, 16> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        uint32_t returned_start = 0;
        uint32_t returned_count = 0;
        auto get_res = co_await ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            returned_start, returned_count);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(returned_start, 3U);
        EXPECT_EQ(returned_count, kTokenCount);
        std::array<uint32_t, kTokenCount> returned{returned_start,
                                                   returned_start + 1,
                                                   returned_start + 2};

        std::array<uint32_t, kTokenCount> to_consumer{returned[2], returned[0],
                                                      returned[1]};
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto consume_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{to_consumer}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  EXPECT_EQ(consumed_payloads,
            (std::vector<AuthTokenPayload>{x23::serialize(2U),
                                           x23::serialize(0U),
                                           x23::serialize(1U)}));
}

// GetCalldata reports the calldata and the number of forwarded tokens, which
// are the frame's ids 0..N-1.
TEST_F(SyscallAuthTokenTest, GetCalldataReportsTheAuthTokenCount)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr uint32_t kTokenCount = 3;
  constexpr std::array<std::byte, 5> kCalldata{std::byte{1}, std::byte{2},
                                               std::byte{3}, std::byte{4},
                                               std::byte{5}};

  std::vector<std::byte> received_calldata;
  uint32_t received_count = 0;
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        std::array<std::byte, 16> calldata_buf{};
        RuntimeMemoryLength written_len = 0;
        auto res = co_await ctx.syscall<SyscallNo::kGetCalldata>(
            std::span<std::byte>{calldata_buf}, written_len, received_count);
        EXPECT_TRUE(res.has_value());
        EXPECT_EQ(res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        received_calldata.assign(calldata_buf.begin(),
                                 calldata_buf.begin() + written_len);
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        std::array<uint32_t, kTokenCount> minted{};
        for (auto &token_id : minted) {
          auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
              BlockHeight{1000},
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              token_id);
          EXPECT_TRUE(mint_res.has_value());
          EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        }

        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{kCalldata},
            std::span<uint32_t const>{minted}, inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  EXPECT_EQ(received_calldata,
            std::vector<std::byte>(kCalldata.begin(), kCalldata.end()));
  EXPECT_EQ(received_count, kTokenCount);
}

// Plain-invoke twin of ReadOnlyInvokeDropsUnreturnedForwardedAuthTokens: a
// forwarded token the child neither consumes nor returns leaves the caller
// when the child commits.
TEST_F(SyscallAuthTokenTest, InvokeDropsUnreturnedForwardedAuthTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        ADD_FAILURE() << "Consumer should never be invoked";
        co_return InvokeStatus<void>{};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Child: receives the token but does not consume or return it.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) -> x23::Async<InvokeStatus<void>> {
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 1> forwarded{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs outputs{};
        auto invoke_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.frame_status, InvocationFrameStatusCode::Return);
        EXPECT_EQ(outputs.returned_auth_token_count, 0U);

        // The child committed without returning the token: the caller no
        // longer owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  contract_map.emplace(2, std::move(consumer_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);
  auto consumer_xdr = make_native_contract(2);
  write_contract(consumer_xdr);
  write_deployment(kConsumerAddress, consumer_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
}

// A TryInvoke child consumes a forwarded token and then faults; the discarded
// frame leaves the token with the caller, who may forward it again.
TEST_F(SyscallAuthTokenTest,
       TryInvokeChildConsumesThenFaultsLeavesTokenWithCaller)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr Runtime::MethodID kConsumeThenFault = 0;
  constexpr Runtime::MethodID kConsumeThenReturn = 1;

  uint32_t first_child_start = 0;
  uint32_t first_child_count = 0;
  uint32_t second_child_start = 0;
  uint32_t second_child_count = 0;

  auto consume_from_calldata = [](NativeSyscallContext &ctx,
                                  uint32_t &child_start,
                                  uint32_t &child_count) -> x23::Async<bool> {
    auto const &calldata = ctx.handler().get_calldata();
    if (calldata.size() != sizeof(uint32_t)) {
      ADD_FAILURE() << "Expected token id in calldata";
      co_return false;
    }
    uint32_t token_id = 0;
    std::memcpy(&token_id, calldata.data(), sizeof(token_id));

    std::array<std::byte, 256> payload_buf{};
    RuntimeMemoryLength payload_written_len = 0;
    ContractAddress minter{};
    auto consume_res = co_await ctx.syscall<SyscallNo::kConsumeAuthToken>(
        token_id, std::span<std::byte>{payload_buf}, minter,
        payload_written_len, child_start, child_count);
    EXPECT_TRUE(consume_res.has_value());
    EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
    co_return consume_res.has_value() &&
        consume_res.value() == SyscallErrorCode::kSyscallErrorCodeNone;
  };

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(kConsumeThenFault,
                    [&](NativeSyscallContext &ctx)
                        -> x23::Async<InvokeStatus<void>> {
                      co_await consume_from_calldata(ctx, first_child_start,
                                                     first_child_count);
                      co_return InvokeError{InvokeErrorCode::Deterministic};
                    });
  fns_child.emplace(
      kConsumeThenReturn,
      [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        if (!co_await consume_from_calldata(ctx, second_child_start,
                                            second_child_count)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // One leaf under the root so consuming it mints a child token.
        AuthTokenNode leaf{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        AuthTokenNode root{.consumer_address = kChildAddress,
                           .payload = x23::serialize(42U),
                           .child_nodes = {leaf}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = co_await ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        Calldata calldata;
        calldata.resize_uninitialized(sizeof(token_id));
        std::memcpy(calldata.data(), &token_id, sizeof(token_id));
        std::array<uint32_t, 1> forwarded{token_id};

        // First: the child consumes the token, then faults.
        CopyableInvokeInputs fault_inputs{
            .entrypoint_id = kConsumeThenFault,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::TryInvoke),
        };
        CopyableInvokeOutputs fault_outputs{};
        auto fault_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            fault_inputs, fault_outputs);
        EXPECT_TRUE(fault_res.has_value());
        EXPECT_EQ(fault_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(fault_outputs.frame_status, InvocationFrameStatusCode::Fault);

        // Second: the caller still owns the token, and the child can consume
        // it again.
        CopyableInvokeInputs retry_inputs{
            .entrypoint_id = kConsumeThenReturn,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs retry_outputs{};
        auto retry_res = co_await ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            retry_inputs, retry_outputs);
        EXPECT_TRUE(retry_res.has_value());
        EXPECT_EQ(retry_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(retry_outputs.frame_status,
                  InvocationFrameStatusCode::Return);

        co_return InvokeStatus<void>{};
      });
  auto caller_contract =
      std::make_unique<NativeContract>(std::move(fns_caller));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(caller_contract));
  contract_map.emplace(1, std::move(child_contract));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto caller_xdr = make_native_contract(0);
  write_contract(caller_xdr);
  write_deployment(kCallerAddress, caller_xdr);
  auto child_xdr = make_native_contract(1);
  write_contract(child_xdr);
  write_deployment(kChildAddress, child_xdr);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{
                          .invoked_address = kCallerAddress,
                          .entrypoint = entrypoint_from_method_id(0),
                          .gas_limit = kDefaultMaxTxGas,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);

  EXPECT_EQ(first_child_count, 1U);
  EXPECT_EQ(second_child_count, 1U);
  EXPECT_EQ(second_child_start, first_child_start);
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
