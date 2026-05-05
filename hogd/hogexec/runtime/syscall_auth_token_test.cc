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
#include "invoke_flag.hh"
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

// Regression test for #1702: a child mints a token then faults; the parent
// must not be able to forward the child-minted token ID to another contract.
TEST_F(SyscallAuthTokenTest, ForwardUnownedAuthTokenAfterChildRevertIsRejected)
{
  constexpr ContractAddress kAttackerAddress = make_contract_address(0);
  constexpr ContractAddress kMinterAddress = make_contract_address(1);
  constexpr ContractAddress kConsumerAddress = make_contract_address(2);

  // Consumer: no-op — should never execute.
  auto consumer_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> {
             ADD_FAILURE() << "Consumer should never be invoked";
             return {};
           }}});

  // Minter: mints a token targeting the consumer, then faults.
  auto minter_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             EXPECT_EQ(mint_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             // Write the minted token ID back to the caller via return buffer
             // before faulting, so the attacker knows which ID to forge.
             std::array<std::byte, sizeof(uint32_t)> ret_buf{};
             std::memcpy(ret_buf.data(), &token_id, sizeof(token_id));
             auto ret = ctx.syscall<SyscallNo::kReturn>(
                 std::span<const std::byte>{ret_buf}, true,
                 std::span<const std::byte>{});
             EXPECT_FALSE(ret.has_value());

             return {};
           }}});

  // Attacker: invokes minter via try-invoke, then tries to forward the
  // minter's token ID to the consumer.
  auto attacker_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // Step 1: invoke minter with TryInvoke — it will mint and fault.
             RuntimeMemoryLength output_return_len = 0,
                                 output_auth_token_len = 0;
             auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
                 kMinterAddress, 0, UINT64_MAX, Calldata{},
                 std::to_underlying(InvokeFlag::TryInvoke),
                 std::span<uint32_t const>{}, output_return_len,
                 output_auth_token_len);
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
             EXPECT_EQ(get_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);
             if (return_written_len != sizeof(uint32_t)) {
               ADD_FAILURE() << "Expected token id in return buffer";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             uint32_t stolen_token_id = 0;
             std::memcpy(&stolen_token_id, return_buf.data(), sizeof(uint32_t));

             // Step 2: try to forward the stolen token ID to the consumer.
             // This must be rejected because the attacker doesn't own it.
             std::array<uint32_t, 1> forged_ids{stolen_token_id};
             RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;
             auto forge_res = ctx.syscall<SyscallNo::kInvoke>(
                 kConsumerAddress, 0, UINT64_MAX, Calldata{}, 0,
                 std::span<uint32_t const>{forged_ids}, out_ret_len,
                 out_auth_len);
             EXPECT_TRUE(forge_res.has_value());
             EXPECT_EQ(forge_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

             return {};
           }}});

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

  auto callee_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> {
             ADD_FAILURE() << "Callee should never be invoked";
             return {};
           }}});

  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             constexpr uint32_t kFabricatedId = 123456;
             std::array<uint32_t, 1> forged_ids{kFabricatedId};
             RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;
             auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
                 kCalleeAddress, 0, UINT64_MAX, Calldata{}, 0,
                 std::span<uint32_t const>{forged_ids}, out_ret_len,
                 out_auth_len);
             EXPECT_TRUE(invoke_res.has_value());
             EXPECT_EQ(invoke_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
             return {};
           }}});

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

             std::array<std::byte, 256> payload_buf{};
             RuntimeMemoryLength payload_written_len = 0;
             uint32_t child_start = 0, child_count = 0;
             auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                 token_id, std::span<std::byte>{payload_buf},
                 payload_written_len, child_start, child_count);
             EXPECT_TRUE(consume_res.has_value());
             EXPECT_EQ(consume_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);
             return {};
           }}});

  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             EXPECT_EQ(mint_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             Calldata calldata;
             calldata.resize_uninitialized(sizeof(token_id));
             std::memcpy(calldata.data(), &token_id, sizeof(token_id));

             // First invoke: child consumes the token successfully.
             std::array<uint32_t, 1> forwarded{token_id};
             RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;
             auto first_res = ctx.syscall<SyscallNo::kInvoke>(
                 kChildAddress, 0, UINT64_MAX, calldata, 0,
                 std::span<uint32_t const>{forwarded}, out_ret_len,
                 out_auth_len);
             EXPECT_TRUE(first_res.has_value());
             EXPECT_EQ(first_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             // Second invoke: parent no longer owns the token.
             out_ret_len = 0;
             out_auth_len = 0;
             auto second_res = ctx.syscall<SyscallNo::kInvoke>(
                 kChildAddress, 0, UINT64_MAX, calldata, 0,
                 std::span<uint32_t const>{forwarded}, out_ret_len,
                 out_auth_len);
             EXPECT_TRUE(second_res.has_value());
             EXPECT_EQ(second_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

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

// Regression test for #1711: a middle contract learns a token ID from calldata
// but does not own it; forwarding it to the consumer must be rejected.
TEST_F(SyscallAuthTokenTest, MiddleContractCannotForwardUnownedToken)
{
  constexpr ContractAddress kCallerAddress = make_contract_address(0);
  constexpr ContractAddress kMiddleAddress = make_contract_address(1);
  constexpr ContractAddress kLeafAddress = make_contract_address(2);

  auto leaf_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext & /*ctx*/) -> InvokeStatus<void> {
             ADD_FAILURE() << "Leaf should never be invoked";
             return {};
           }}});

  // Middle: receives the token ID in calldata (not forwarded), tries to
  // forward it to the leaf.
  auto middle_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto const &calldata = ctx.handler().get_calldata();
             if (calldata.size() != sizeof(uint32_t)) {
               ADD_FAILURE() << "Expected token id in calldata";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             uint32_t token_id = 0;
             std::memcpy(&token_id, calldata.data(), sizeof(token_id));

             std::array<uint32_t, 1> stolen_ids{token_id};
             RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;
             auto invoke_res = ctx.syscall<SyscallNo::kInvoke>(
                 kLeafAddress, 0, UINT64_MAX, Calldata{}, 0,
                 std::span<uint32_t const>{stolen_ids}, out_ret_len,
                 out_auth_len);
             EXPECT_TRUE(invoke_res.has_value());
             EXPECT_EQ(invoke_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
             return {};
           }}});

  // Caller: mints a token targeting the leaf, then invokes the middle
  // WITHOUT forwarding the token — only passes the ID in calldata.
  auto caller_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
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
             EXPECT_EQ(mint_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             // Pass the token ID in calldata, but do NOT forward it.
             Calldata calldata;
             calldata.resize_uninitialized(sizeof(token_id));
             std::memcpy(calldata.data(), &token_id, sizeof(token_id));

             RuntimeMemoryLength out_ret_len = 0, out_auth_len = 0;
             auto invoke_res =
                 ctx.syscall<SyscallNo::kInvoke>(kMiddleAddress, 0, UINT64_MAX,
                                                 calldata, 0,
                                                 std::span<uint32_t const>{},
                                                 out_ret_len, out_auth_len);
             EXPECT_TRUE(invoke_res.has_value());
             EXPECT_EQ(invoke_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);
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

TEST_F(SyscallAuthTokenTest, MintAuthTokenRejectsOverDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  auto contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             AuthTokenNode node{.consumer_address = kAddr,
                                .payload = x23::serialize(
                                    uint32_t{kAuthTokenMaxDepth}),
                                .child_nodes = {}};
             for (uint32_t level = kAuthTokenMaxDepth; level > 0; --level) {
               node =
                   AuthTokenNode{.consumer_address = kAddr,
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

TEST_F(SyscallAuthTokenTest, MintAuthTokenAcceptsMaxDepthTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  auto contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             AuthTokenNode node{.consumer_address = kAddr,
                                .payload = x23::serialize(
                                    uint32_t{kAuthTokenMaxDepth - 1}),
                                .child_nodes = {}};
             for (uint32_t level = kAuthTokenMaxDepth - 1; level > 0; --level) {
               node =
                   AuthTokenNode{.consumer_address = kAddr,
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
             EXPECT_EQ(mint_res.value(),
                       SyscallErrorCode::kSyscallErrorCodeNone);

             uint32_t token_id = root_id;
             for (uint32_t level = 0; level < uint32_t{kAuthTokenMaxDepth};
                  ++level) {
               std::array<std::byte, 256> payload_buf{};
               RuntimeMemoryLength payload_written_len = 0;
               uint32_t child_start = 0, child_count = 0;
               auto consume_res = ctx.syscall<SyscallNo::kConsumeAuthToken>(
                   token_id, std::span<std::byte>{payload_buf},
                   payload_written_len, child_start, child_count);
               EXPECT_TRUE(consume_res.has_value());
               EXPECT_EQ(consume_res.value(),
                         SyscallErrorCode::kSyscallErrorCodeNone);

               auto expected_payload = x23::serialize(level);
               EXPECT_EQ(payload_written_len, expected_payload.size());
               EXPECT_EQ(std::memcmp(payload_buf.data(),
                                     expected_payload.data(),
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

TEST_F(SyscallAuthTokenTest, MintAuthTokenRejectsOverDepthBranchingTree)
{
  constexpr ContractAddress kAddr = make_contract_address(0);

  auto contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [&](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             AuthTokenNode too_deep{.consumer_address = kAddr,
                                    .payload = x23::serialize(uint32_t{5}),
                                    .child_nodes = {}};
             AuthTokenNode great_grandchild{.consumer_address = kAddr,
                                            .payload =
                                                x23::serialize(uint32_t{4}),
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

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
