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
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

class SyscallAuthTokenTest : public ::testing::Test,
                             public NativeContractTestFramework {
public:
  SyscallAuthTokenTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(SyscallAuthTokenTest, InvokeForwardsOwnedAuthTokens)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kMiddleAddress = make_contract_address(1);
  constexpr ContractAddress kLeafAddress = make_contract_address(2);

  auto leaf_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> {
             return {};
           }}});

  auto middle_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto const &calldata = ctx.handler().get_calldata();
             if (calldata.size() != sizeof(uint32_t)) {
               ADD_FAILURE() << "Expected token id calldata";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             uint32_t token_id = 0;
             std::memcpy(&token_id, calldata.data(), sizeof(token_id));
             std::array<uint32_t, 1> forwarded_token_ids{token_id};

             RuntimeMemoryLength output_return_buffer_len = 0,
                                 output_returned_auth_token_len = 0;
             auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
                 kLeafAddress, 0, UINT64_MAX, Calldata{}, 0,
                 std::span<uint32_t const>{forwarded_token_ids},
                 output_return_buffer_len, output_returned_auth_token_len);
             if (!invoke_res.has_value() ||
                 invoke_res.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Middle->Leaf invoke failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             RuntimeMemoryLength output_return_buffer_len = 0,
                                 output_returned_auth_token_len = 0;
             auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
                 kMiddleAddress, 0, UINT64_MAX, calldata, 0,
                 std::span<uint32_t const>{owned_token_ids},
                 output_return_buffer_len, output_returned_auth_token_len);
             if (!invoke_res.has_value() ||
                 invoke_res.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Caller->Middle invoke failed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

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
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

TEST_F(SyscallAuthTokenTest, ConsumeNestedAuthTokens)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  auto contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             EXPECT_EQ(mint_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             // Consume root -> payload=42, 2 children
             std::array<std::byte, 256> payload_buf{};
             uint32_t child_start = 0, child_count = 0;
             RuntimeMemoryLength payload_written_len = 0;
             auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 root_id, std::span<std::byte>{payload_buf},
                 payload_written_len, child_start, child_count);
             EXPECT_TRUE(consume_res.has_value());
             EXPECT_EQ(consume_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);
             EXPECT_EQ(child_count, 2U);
             auto expected_root_payload = x23::serialize(42U);
             EXPECT_EQ(std::memcmp(payload_buf.data(),
                                   expected_root_payload.data(),
                                   expected_root_payload.size()),
                       0);
             EXPECT_EQ(payload_written_len, expected_root_payload.size());

             // Consume child0 -> payload=100, 1 grandchild
             std::array<std::byte, 256> c0_payload{};
             RuntimeMemoryLength c0_payload_written_len = 0;
             uint32_t gc_start = 0, gc_count = 0;
             auto c0_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 child_start, std::span<std::byte>{c0_payload},
                 c0_payload_written_len, gc_start, gc_count);
             EXPECT_TRUE(c0_res.has_value());
             EXPECT_EQ(c0_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
             EXPECT_EQ(gc_count, 1U);
             auto expected_c0_payload = x23::serialize(100U);
             EXPECT_EQ(std::memcmp(c0_payload.data(),
                                   expected_c0_payload.data(),
                                   expected_c0_payload.size()),
                       0);

             // Consume child1 -> payload=300, 0 grandchildren
             std::array<std::byte, 256> c1_payload{};
             RuntimeMemoryLength c1_payload_written_len = 0;
             uint32_t unused_start = 0, c1_count = 0;
             auto c1_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 child_start + 1, std::span<std::byte>{c1_payload},
                 c1_payload_written_len, unused_start, c1_count);
             EXPECT_TRUE(c1_res.has_value());
             EXPECT_EQ(c1_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
             EXPECT_EQ(c1_count, 0U);
             auto expected_c1_payload = x23::serialize(300U);
             EXPECT_EQ(std::memcmp(c1_payload.data(),
                                   expected_c1_payload.data(),
                                   expected_c1_payload.size()),
                       0);
             EXPECT_EQ(c1_payload_written_len, expected_c1_payload.size());

             // Consume grandchild -> payload=200, 0 children
             std::array<std::byte, 256> gc_payload{};
             RuntimeMemoryLength gc_payload_written_len = 0;
             uint32_t gc_unused_start = 0, gc_gc_count = 0;
             auto gc_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 gc_start, std::span<std::byte>{gc_payload},
                 gc_payload_written_len, gc_unused_start, gc_gc_count);
             EXPECT_TRUE(gc_res.has_value());
             EXPECT_EQ(gc_res.value(), SyscallErrorCode::kSyscallErrorCodeNone);
             EXPECT_EQ(gc_gc_count, 0U);
             auto expected_gc_payload = x23::serialize(200U);
             EXPECT_EQ(std::memcmp(gc_payload.data(),
                                   expected_gc_payload.data(),
                                   expected_gc_payload.size()),
                       0);
             EXPECT_EQ(gc_payload_written_len, expected_gc_payload.size());

             return {};
           }}});

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
};

TEST_F(SyscallAuthTokenExpiryTest, ConsumeAuthTokenExpiryBoundary)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  advance_block();

  auto contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             BlockHeight current_height{};
             auto h_res =
                 ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
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
                   std::span<const std::byte>{
                       reinterpret_cast<const std::byte *>(serialized.data()),
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
               auto r = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                   id, std::span<std::byte>{buf}, w, s, c);
               EXPECT_TRUE(r.has_value());
               EXPECT_EQ(r.value(), SyscallErrorCode::kSyscallErrorCodeNone);
               EXPECT_EQ(w, x23::serialize(1U).size());
             }

             return {};
           }}});

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

  auto contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             BlockHeight current_height{};
             auto h_res =
                 ctx.syscall<SyscallNo::kGetBlockHeight>(current_height);
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
                   std::span<const std::byte>{
                       reinterpret_cast<const std::byte *>(serialized.data()),
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
             EXPECT_EQ(present.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             // future height: accepted
             auto future = try_mint(BlockHeight{current_height + 1});
             EXPECT_TRUE(future.has_value());
             EXPECT_EQ(future.value(), SyscallErrorCode::kSyscallErrorCodeNone);

             return {};
           }}});

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
  auto child_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 token_id, std::span<std::byte>{payload_buf},
                 payload_written_len, child_start, child_count);
             EXPECT_TRUE(consume_res.has_value());
             EXPECT_EQ(consume_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);
             EXPECT_EQ(child_count, 1U);

             // Build the returned_auth_token_ids span (the unwrapped child)
             std::array<uint32_t, 1> returned_ids{child_start};
             auto auth_bytes =
                 std::as_bytes(std::span<const uint32_t>{returned_ids});

             // Call Return via ctx.syscall to go through the full path
             std::array<std::byte, 4> ret_buf{};
             std::memcpy(ret_buf.data(), "OK!!", 4);
             auto ret = ctx.syscall<SyscallNo::kReturn>(
                 std::span<const std::byte>{ret_buf}, false, auth_bytes);
             // Return always yields HostFnError::Return (non-recoverable)
             EXPECT_FALSE(ret.has_value());

             return {};
           }}});

  // Caller: mints a nested token (root → child), invokes child forwarding
  // the root, then tries to consume the returned child token.
  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             EXPECT_EQ(mint_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             // Invoke child, forwarding the root token
             Calldata calldata;
             calldata.resize_uninitialized(sizeof(root_id));
             std::memcpy(calldata.data(), &root_id, sizeof(root_id));

             std::array<uint32_t, 1> forwarded{root_id};
             RuntimeMemoryLength output_return_len = 0,
                                 output_auth_token_len = 0;
             auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
                 kChildAddress, 0, UINT64_MAX, calldata, 0,
                 std::span<uint32_t const>{forwarded}, output_return_len,
                 output_auth_token_len);
             EXPECT_TRUE(invoke_res.has_value());
             EXPECT_EQ(invoke_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             uint32_t leaf_id = root_id + 1;

             // Issue 3: Invoke should report the returned auth token byte len
             EXPECT_EQ(output_auth_token_len, sizeof(uint32_t));

             // Issue 2: GetReturnedBuffer should write the auth token IDs
             std::array<std::byte, 256> return_buf{};
             RuntimeMemoryLength return_written_len = 0;
             std::array<std::byte, 256> auth_token_buf{};
             RuntimeMemoryLength auth_token_written_len = 0;
             auto get_res = ctx.syscall<SyscallNo::kGetReturnedBuffer>(
                 std::span<std::byte>{return_buf}, return_written_len,
                 std::span<std::byte>{auth_token_buf}, auth_token_written_len);
             EXPECT_TRUE(get_res.has_value());
             EXPECT_EQ(get_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);
             EXPECT_EQ(return_written_len, 4U);
             EXPECT_EQ(auth_token_written_len, sizeof(uint32_t));
             if (auth_token_written_len == sizeof(uint32_t)) {
               uint32_t returned_id = 0;
               std::memcpy(&returned_id, auth_token_buf.data(),
                           sizeof(uint32_t));
               EXPECT_EQ(returned_id, leaf_id);
             }

             // The caller should own the returned token and be able to
             // consume it.
             std::array<std::byte, 256> leaf_payload{};
             RuntimeMemoryLength leaf_payload_len = 0;
             uint32_t unused_start = 0, unused_count = 0;
             auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 leaf_id, std::span<std::byte>{leaf_payload}, leaf_payload_len,
                 unused_start, unused_count);
             EXPECT_TRUE(consume_res.has_value());
             EXPECT_EQ(consume_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             if (consume_res.has_value() &&
                 consume_res.value() ==
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               auto expected_payload = x23::serialize(99U);
               EXPECT_EQ(leaf_payload_len, expected_payload.size());
               EXPECT_EQ(std::memcmp(leaf_payload.data(),
                                     expected_payload.data(),
                                     expected_payload.size()),
                         0);
             }

             return {};
           }}});

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

  auto child_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // Claim to return token ID 9999, which we don't own.
             std::array<uint32_t, 1> bogus_ids{9999};
             auto auth_bytes =
                 std::as_bytes(std::span<const uint32_t>{bogus_ids});

             std::array<std::byte, 4> buf{};
             auto ret = ctx.syscall<SyscallNo::kReturn>(
                 std::span<const std::byte>{buf}, false, auth_bytes);
             EXPECT_FALSE(ret.has_value());
             EXPECT_EQ(ret.error().code(), HostFnErrorCode::Exit);

             return InvokeError{InvokeErrorCode::Deterministic};
           }}});

  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             RuntimeMemoryLength output_return_len = 0,
                                 output_auth_token_len = 0;
             auto invoke_res =
                 ctx.syscall<SyscallNo::kInvoke>(kChildAddress, 0, UINT64_MAX,
                                                 Calldata{}, 0,
                                                 std::span<uint32_t const>{},
                                                 output_return_len,
                                                 output_auth_token_len);
             EXPECT_TRUE(invoke_res.has_value());
             EXPECT_EQ(invoke_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeDeterministicWasm);
             return {};
           }}});

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
              }},
      *runtime_context);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
