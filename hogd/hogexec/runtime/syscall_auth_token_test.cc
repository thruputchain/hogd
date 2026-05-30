#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "auth_token.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

class SyscallAuthTokenTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  SyscallAuthTokenTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
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
  fns_leaf.emplace(0,
                   [](NativeSyscallContext & /*ctx*/) noexcept
                       -> InvokeStatus<void> { return {}; });
  auto leaf_contract = std::make_unique<NativeContract>(std::move(fns_leaf));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_middle;
  fns_middle.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id calldata";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));
        std::array<uint32_t, 1> forwarded_token_ids{token_id};

        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kLeafAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded_token_ids}, inputs,
            CopyableInvokeOutputs{});
        if (!invoke_res.has_value() ||
            invoke_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Middle->Leaf invoke failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto middle_contract =
      std::make_unique<NativeContract>(std::move(fns_middle));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kMiddleAddress,
                                .payload = x23::serialize(7U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        if (!mint_res.has_value() ||
            mint_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "MintAuthToken failed";
          return InvokeError{InvokeErrorCode::Deterministic};
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
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kMiddleAddress, calldata,
                                            std::span<uint32_t const>{
                                                owned_token_ids},
                                            inputs, CopyableInvokeOutputs{});
        if (!invoke_res.has_value() ||
            invoke_res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Caller->Middle invoke failed";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, InvokeRejectsDuplicateForwardedAuthTokenIds)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "Child should not be invoked";
        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(7U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{duplicate_token_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, GetCalldataRejectsPartialAuthTokenOutputSpan)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        RuntimeMemoryLength calldata_len = 0;
        uint32_t auth_token_count = 0;
        auto len_res =
            ctx.syscall<SyscallNo::kGetCalldataLen>(calldata_len,
                                                    auth_token_count);
        EXPECT_TRUE(len_res.has_value());
        EXPECT_EQ(len_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(auth_token_count, 1U);

        for (RuntimeMemoryLength short_len = 1; short_len < sizeof(uint32_t);
             ++short_len) {
          std::array<std::byte, sizeof(uint32_t)> calldata_buf{};
          RuntimeMemoryLength written_len = 0;
          std::array<std::byte, sizeof(uint32_t)> auth_token_buf{};
          RuntimeMemoryLength auth_token_written_len = 0;

          auto get_res = ctx.syscall<SyscallNo::kGetCalldata>(
              std::span<std::byte>{calldata_buf}, written_len,
              std::span<std::byte>{auth_token_buf.data(), short_len},
              auth_token_written_len);
          EXPECT_TRUE(get_res.has_value());
          EXPECT_EQ(get_res.value(),
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        }

        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(7U),
                                .child_nodes = {}};
        auto serialized_root = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized_root.data()),
                                       serialized_root.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 1> forwarded_token_ids{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded_token_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ConsumeNestedAuthTokens)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
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
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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
        auto c0_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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
        auto c1_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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
        auto gc_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

class SyscallAuthTokenExpiryTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  SyscallAuthTokenExpiryTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
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

TEST_F(SyscallAuthTokenExpiryTest, ConsumeAuthTokenExpiryBoundary)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        BlockHeight current_height{};
        auto h_res = ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
        EXPECT_TRUE(h_res.has_value());
        EXPECT_EQ(h_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_GT(current_height, BlockHeight{0});

        auto make_and_mint = [&](BlockHeight expiry) -> uint32_t {
          AuthTokenNode node{.consumer_address = kAddr,
                             .payload = x23::serialize(1U),
                             .child_nodes = {}};
          auto serialized = x23::serialize(node);
          uint32_t id = 0;
          auto r = ctx.syscall<SyscallNo::kMintAuthToken>(
              expiry,
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              id);
          EXPECT_TRUE(r.has_value());
          EXPECT_EQ(r.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          return id;
        };

        // expiry == block height: token is NOT expired (< is false)
        {
          uint32_t id = make_and_mint(current_height);
          std::array<std::byte, 256> buf{};
          RuntimeMemoryLength w = 0;
          uint32_t s = 0, c = 0;
          ContractAddress m{};
          auto r = ctx.syscall<SyscallNo::kConsumeAuthToken>(
              id, std::span<std::byte>{buf}, m, w, s, c);
          EXPECT_TRUE(r.has_value());
          EXPECT_EQ(r.value(), SyscallErrorCode::kSyscallErrorCodeNone);
          EXPECT_EQ(w, x23::serialize(1U).size());
        }

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenExpiryTest, MintAuthTokenExpiryBoundary)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        BlockHeight current_height{};
        auto h_res = ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
        EXPECT_TRUE(h_res.has_value());
        EXPECT_EQ(h_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_GT(current_height, BlockHeight{0});

        auto try_mint = [&](BlockHeight expiry) {
          AuthTokenNode node{.consumer_address = kAddr,
                             .payload = x23::serialize(1U),
                             .child_nodes = {}};
          auto serialized = x23::serialize(node);
          uint32_t id = 0;
          return ctx.syscall<SyscallNo::kMintAuthToken>(
              expiry,
              std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                             serialized.data()),
                                         serialized.size()},
              id);
        };

        // past expiry: rejected
        auto past = try_mint(BlockHeight{current_height - 1});
        EXPECT_TRUE(past.has_value());
        EXPECT_EQ(past.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        // current height: accepted (< is false)
        auto present = try_mint(current_height);
        EXPECT_TRUE(present.has_value());
        EXPECT_EQ(present.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // future height: accepted
        auto future = try_mint(BlockHeight{current_height + 1});
        EXPECT_TRUE(future.has_value());
        EXPECT_EQ(future.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}
// Child consumes a token, gets unwrapped child tokens, returns them to
// the parent via the Return syscall.  Parent then consumes the returned token.
//
// This exercises the full round-trip: mint → forward → consume → Return
// with returned_auth_token_ids → parent owns the returned tokens.
//
// EXPECTED TO FAIL until set_child_restore_auth_tokens is called on the
// parent frame (not the child frame) in set_return_buffer.
TEST_F(SyscallAuthTokenTest, ReturnAuthTokensToParent)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // Child: receives a forwarded token via calldata, consumes it,
  // then returns the unwrapped child tokens to the parent via Return.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        // Consume the token → get payload + unwrapped child tokens
        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        uint32_t child_start = 0, child_count = 0;
        ContractAddress minter{};
        auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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
        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{ret_buf},
                                            false, auth_bytes);
        // Return always yields HostFnError::Return (non-recoverable)
        EXPECT_FALSE(ret.has_value());

        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints a nested token (root → child), invokes child forwarding
  // the root, then tries to consume the returned child token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
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
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        uint32_t leaf_id = root_id + 1;

        // Issue 3: Invoke should report the returned auth token byte len
        EXPECT_EQ(outputs.returned_auth_token_len, sizeof(uint32_t));

        // Issue 2: GetReturnedBuffer should write the auth token IDs
        std::array<std::byte, 256> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        std::array<std::byte, 256> auth_token_buf{};
        RuntimeMemoryLength auth_token_written_len = 0;
        auto get_res = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            std::span<std::byte>{auth_token_buf}, auth_token_written_len);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(return_written_len, 4U);
        EXPECT_EQ(auth_token_written_len, sizeof(uint32_t));
        if (auth_token_written_len == sizeof(uint32_t)) {
          uint32_t returned_id = 0;
          std::memcpy(&returned_id, auth_token_buf.data(), sizeof(uint32_t));
          EXPECT_EQ(returned_id, leaf_id);
        }

        // The caller should own the returned token and be able to
        // consume it.
        std::array<std::byte, 256> leaf_payload{};
        RuntimeMemoryLength leaf_payload_len = 0;
        uint32_t unused_start = 0, unused_count = 0;
        ContractAddress leaf_minter{};
        auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReturnRejectsDuplicateAuthTokenIds)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode node{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{ret_buf},
                                            false, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);

        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kChildAddress,
                                            std::span<const std::byte>{},
                                            std::span<uint32_t const>{}, inputs,
                                            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);
        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, GetReturnedBufferRejectsPartialAuthTokenOutputSpan)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode node{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        std::array<uint32_t, 1> returned_ids{token_id};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{returned_ids});
        std::array<std::byte, 4> ret_buf{};
        std::memcpy(ret_buf.data(), "OK!!", 4);
        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{ret_buf},
                                            false, auth_bytes);
        EXPECT_FALSE(ret.has_value());

        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        CopyableInvokeOutputs outputs{};
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kChildAddress,
                                            std::span<const std::byte>{},
                                            std::span<uint32_t const>{}, inputs,
                                            outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.returned_auth_token_len, sizeof(uint32_t));

        for (RuntimeMemoryLength short_len = 1; short_len < sizeof(uint32_t);
             ++short_len) {
          std::array<std::byte, 4> return_buf{};
          RuntimeMemoryLength return_written_len = 0;
          std::array<std::byte, sizeof(uint32_t)> auth_token_buf{};
          RuntimeMemoryLength auth_token_written_len = 0;
          auto get_res = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
              std::span<std::byte>{return_buf}, return_written_len,
              std::span<std::byte>{auth_token_buf.data(), short_len},
              auth_token_written_len);
          EXPECT_TRUE(get_res.has_value());
          EXPECT_EQ(get_res.value(),
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        }

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Return syscall with auth token IDs that the child doesn't own must
// cause a deterministic exit (fault), not silently succeed.
TEST_F(SyscallAuthTokenTest, ReturnUnownedAuthTokensCausesExit)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Claim to return token ID 9999, which we don't own.
        std::array<uint32_t, 1> bogus_ids{9999};
        auto auth_bytes = std::as_bytes(std::span<const uint32_t>{bogus_ids});

        std::array<std::byte, 4> buf{};
        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{buf},
                                            false, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);

        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kChildAddress,
                                            std::span<const std::byte>{},
                                            std::span<uint32_t const>{}, inputs,
                                            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);
        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "Consumer should never be invoked";
        return {};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Minter: mints a token targeting the consumer, then faults.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_minter;
  fns_minter.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kConsumerAddress,
                                .payload = x23::serialize(0xDEADU),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        // Write the minted token ID back to the caller via return buffer
        // before faulting, so the attacker knows which ID to forge.
        std::array<std::byte, sizeof(uint32_t)> ret_buf{};
        std::memcpy(ret_buf.data(), &token_id, sizeof(token_id));
        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{ret_buf},
                                            true, std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());

        return {};
      });
  auto minter_contract =
      std::make_unique<NativeContract>(std::move(fns_minter));

  // Attacker: invokes minter via try-invoke, then tries to forward the
  // minter's token ID to the consumer.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_attacker;
  fns_attacker.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Step 1: invoke minter with TryInvoke — it will mint and fault.
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::TryInvoke),
        };
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kMinterAddress,
                                            std::span<const std::byte>{},
                                            std::span<uint32_t const>{}, inputs,
                                            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeFrameReverted);

        // Read the token ID the minter wrote into its return buffer.
        std::array<std::byte, 256> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        std::array<std::byte, 256> auth_token_buf{};
        RuntimeMemoryLength auth_token_written_len = 0;
        auto get_res = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            std::span<std::byte>{auth_token_buf}, auth_token_written_len);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        if (return_written_len != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in return buffer";
          return InvokeError{InvokeErrorCode::Deterministic};
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
        auto forge_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forged_ids}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(forge_res.has_value());
        EXPECT_EQ(forge_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Regression test for #1702 comment 1: forwarding a completely fabricated
// token ID (never minted) must be rejected, not crash via CHECK_CONST_PTR.
TEST_F(SyscallAuthTokenTest, ForwardFabricatedAuthTokenIdIsRejected)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kCalleeAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_callee;
  fns_callee.emplace(
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "Callee should never be invoked";
        return {};
      });
  auto callee_contract =
      std::make_unique<NativeContract>(std::move(fns_callee));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        constexpr uint32_t kFabricatedId = 123456;
        std::array<uint32_t, 1> forged_ids{kFabricatedId};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kCalleeAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forged_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Regression test for #1702 comment 2: after a child consumes a token, the
// parent must not be able to re-forward that consumed token ID.
TEST_F(SyscallAuthTokenTest, ReplayConsumedAuthTokenIsRejected)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        uint32_t child_start = 0, child_count = 0;
        ContractAddress minter{};
        auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, child_start, child_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root{.consumer_address = kChildAddress,
                           .payload = x23::serialize(42U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto first_res = ctx.syscall<SyscallNo::kInvoke>(
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
        auto second_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs2, CopyableInvokeOutputs{});
        EXPECT_TRUE(second_res.has_value());
        EXPECT_EQ(second_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "Leaf should never be invoked";
        return {};
      });
  auto leaf_contract = std::make_unique<NativeContract>(std::move(fns_leaf));

  // Middle: receives the token ID in calldata (not forwarded), tries to
  // forward it to the leaf.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_middle;
  fns_middle.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<uint32_t, 1> stolen_ids{token_id};
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kLeafAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{stolen_ids}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        return {};
      });
  auto middle_contract =
      std::make_unique<NativeContract>(std::move(fns_middle));

  // Caller: mints a token targeting the leaf, then invokes the middle
  // WITHOUT forwarding the token — only passes the ID in calldata.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root{.consumer_address = kLeafAddress,
                           .payload = x23::serialize(77U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kMiddleAddress, calldata,
                                            std::span<uint32_t const>{}, inputs,
                                            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, MintAuthTokenRejectsOverDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode node{.consumer_address = kAddr,
                           .payload =
                               x23::serialize(uint32_t{kAuthTokenMaxDepth}),
                           .child_nodes = {}};
        for (uint32_t level = kAuthTokenMaxDepth; level > 0; --level) {
          node = AuthTokenNode{.consumer_address = kAddr,
                               .payload = x23::serialize(level - uint32_t{1}),
                               .child_nodes = {node}};
        }

        auto serialized = x23::serialize(node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, MintAuthTokenAcceptsMaxDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
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
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
          auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
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

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, MintAuthTokenRejectsOverDepthBranchingTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_contract;
  fns_contract.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
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
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        ADD_FAILURE() << "Consumer should never be invoked";
        return {};
      });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: receives the token but does not consume or return it.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(0,
                    [](NativeSyscallContext & /*ctx*/) noexcept
                        -> InvokeStatus<void> { return {}; });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints a token, forwards it to the read-only child, then tries
  // to forward the same token to the consumer (should fail).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // Mint a token targeting the child.
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
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
        auto reuse_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildReturnsForwardedAuthToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op, just proves the caller still owns the token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(0,
                       [](NativeSyscallContext & /*ctx*/) noexcept
                           -> InvokeStatus<void> { return {}; });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: explicitly returns the forwarded token via kReturn.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<uint32_t, 1> returned_ids{token_id};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{returned_ids});
        auto ret = ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{},
                                                   false, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints token, forwards to read-only child that returns it,
  // then forwards it again to consumer (should succeed).
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        CopyableInvokeOutputs outputs{};
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, outputs);
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(outputs.returned_auth_token_len, sizeof(uint32_t));

        // Verify the returned token ID matches what we forwarded.
        std::array<std::byte, 256> return_buf{};
        RuntimeMemoryLength return_written_len = 0;
        std::array<std::byte, 256> auth_token_buf{};
        RuntimeMemoryLength auth_token_written_len = 0;
        auto get_res = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
            std::span<std::byte>{return_buf}, return_written_len,
            std::span<std::byte>{auth_token_buf}, auth_token_written_len);
        EXPECT_TRUE(get_res.has_value());
        EXPECT_EQ(get_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(auth_token_written_len, sizeof(uint32_t));
        uint32_t returned_id = 0;
        std::memcpy(&returned_id, auth_token_buf.data(), sizeof(uint32_t));
        EXPECT_EQ(returned_id, token_id);

        // Token was explicitly returned — caller still owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildCannotConsumeForwardedToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // Read-only child: tries to consume the forwarded token, should be rejected.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<std::byte, 256> payload_buf{};
        RuntimeMemoryLength payload_written_len = 0;
        uint32_t child_start = 0, child_count = 0;
        ContractAddress minter{};
        auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
            token_id, std::span<std::byte>{payload_buf}, minter,
            payload_written_len, child_start, child_count);
        EXPECT_TRUE(consume_res.has_value());
        EXPECT_EQ(consume_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildCannotMintToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  // Read-only child: tries to mint a token, should be rejected.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode node{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(99U),
                           .child_nodes = {}};
        auto serialized = x23::serialize(node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = std::to_underlying(InvokeFlag::ReadOnly),
        };
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kChildAddress,
                                            std::span<const std::byte>{},
                                            std::span<uint32_t const>{}, inputs,
                                            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildFaultRestoresForwardedTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op, proves the caller still owns the token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(0,
                       [](NativeSyscallContext & /*ctx*/) noexcept
                           -> InvokeStatus<void> { return {}; });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: faults deterministically.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext & /*ctx*/) noexcept -> InvokeStatus<void> {
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints token, forwards to read-only child via TryInvoke+ReadOnly,
  // child faults, caller verifies token is restored.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);

        // Token was restored after the fault — caller still owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ReadOnlyChildManualRevertRestoresForwardedTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op, proves the caller still owns the token.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_consumer;
  fns_consumer.emplace(0,
                       [](NativeSyscallContext & /*ctx*/) noexcept
                           -> InvokeStatus<void> { return {}; });
  auto consumer_contract =
      std::make_unique<NativeContract>(std::move(fns_consumer));

  // Read-only child: manually reverts via kReturn with revert_frame=true.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{}, true,
                                            std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());
        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  // Caller: mints token, forwards to read-only child that manually reverts,
  // caller verifies token is restored.
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root_node{.consumer_address = kChildAddress,
                                .payload = x23::serialize(42U),
                                .child_nodes = {}};
        auto serialized = x23::serialize(root_node);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeFrameReverted);

        // Token was restored after the manual revert — caller still owns it.
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto reuse_res = ctx.syscall<SyscallNo::kInvoke>(
            kConsumerAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(reuse_res.has_value());
        EXPECT_EQ(reuse_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// Return(revert_frame=true) with auth token IDs must trap, even when
// the child legitimately owns the token (minted by the parent).
TEST_F(SyscallAuthTokenTest, ReturnRevertWithMintedAuthTokenCausesExit)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kChildAddress = make_contract_address(1);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        auto const &calldata = ctx.handler().get_calldata();
        if (calldata.size() != sizeof(uint32_t)) {
          ADD_FAILURE() << "Expected token id in calldata";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        uint32_t token_id = 0;
        std::memcpy(&token_id, calldata.data(), sizeof(token_id));

        std::array<uint32_t, 1> returned_ids{token_id};
        auto auth_bytes =
            std::as_bytes(std::span<const uint32_t>{returned_ids});
        auto ret = ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{},
                                                   true, auth_bytes);
        EXPECT_FALSE(ret.has_value());
        EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);
        return InvokeError{InvokeErrorCode::Deterministic};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root{.consumer_address = kChildAddress,
                           .payload = x23::serialize(0xBEEFU),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
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
        auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
            kChildAddress, calldata, std::span<uint32_t const>{forwarded},
            inputs, CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);
        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
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
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        AuthTokenNode root{.consumer_address = kCallerAddress,
                           .payload = x23::serialize(0xCAFEU),
                           .child_nodes = {}};
        auto serialized = x23::serialize(root);
        uint32_t token_id = 0;
        auto mint_res = ctx.syscall<SyscallNo::kMintAuthToken>(
            BlockHeight{1000},
            std::span<const std::byte>{reinterpret_cast<const std::byte *>(
                                           serialized.data()),
                                       serialized.size()},
            token_id);
        EXPECT_TRUE(mint_res.has_value());
        EXPECT_EQ(mint_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);

        child_minted_token_id = token_id;

        auto ret =
            ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{}, true,
                                            std::span<const std::byte>{});
        EXPECT_FALSE(ret.has_value());
        return {};
      });
  auto child_contract = std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_leaf;
  fns_leaf.emplace(0,
                   [](NativeSyscallContext &) noexcept -> InvokeStatus<void> {
                     return {};
                   });
  auto leaf_contract = std::make_unique<NativeContract>(std::move(fns_leaf));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_caller;
  fns_caller.emplace(
      0, [&](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        CopyableInvokeInputs inputs{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto invoke_res =
            ctx.syscall<SyscallNo::kInvoke>(kChildAddress,
                                            std::span<const std::byte>{},
                                            std::span<uint32_t const>{}, inputs,
                                            CopyableInvokeOutputs{});
        EXPECT_TRUE(invoke_res.has_value());
        EXPECT_EQ(invoke_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeFrameReverted);

        EXPECT_NE(child_minted_token_id, UINT32_MAX);

        std::array<uint32_t, 1> forwarded{child_minted_token_id};
        CopyableInvokeInputs inputs2{
            .entrypoint_id = 0,
            .gas_limit = UINT64_MAX,
            .flags = 0,
        };
        auto forward_res = ctx.syscall<SyscallNo::kInvoke>(
            kLeafAddress, std::span<const std::byte>{},
            std::span<uint32_t const>{forwarded}, inputs2,
            CopyableInvokeOutputs{});
        EXPECT_TRUE(forward_res.has_value());
        EXPECT_EQ(forward_res.value(),
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
        return {};
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
                          .gas_limit = UINT64_MAX,
                          .calldata = {},
                      },
                  .uniquifier = make_new_uniquifier(),
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
