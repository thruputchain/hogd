#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "auth_token.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
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

class SyscallAuthTokenTest : public ::testing::TestWithParam<TestDB>,
                             public NativeContractTestFramework {
public:
  SyscallAuthTokenTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};


// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_P(SyscallAuthTokenTest, InvokeForwardsOwnedAuthTokens)
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

INSTANTIATE_TEST_SUITE_P(TestDBs, SyscallAuthTokenTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
