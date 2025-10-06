#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/event/event.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class EventTest : public ::testing::Test, public NativeContractTestFramework {
public:
  EventTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  void SetUp() override {}

  static x23::Vec<uint8_t, 0xffffffff> make_vec(std::string_view data)
  {
    return x23::Vec<uint8_t, 0xffffffff>(
        reinterpret_cast<uint8_t const *>(data.data()),
        reinterpret_cast<uint8_t const *>(data.data() + data.size()));
  }
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(EventTest, EventSuccess)
{
  // A native contract with the following methods:
  // 0: Produces an event with 0 topics
  // 1: Produces an event with 1 topic
  // 2: Invokes method 0 and then method 1
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<MethodID, NativeContract::function_t>{
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             const std::string event_data = "test 0 topics";

             auto [status, gas] =
                 syscall_LogEvent_inner(handler, nullptr, nullptr, nullptr,
                                        std::as_bytes(std::span(event_data)));
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {1,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             const std::string event_data = "test 1 topic";

             SHA256Hash topic = sha256(std::string("topic"));

             auto [status, gas] =
                 syscall_LogEvent_inner(handler, &topic, nullptr, nullptr,
                                        std::as_bytes(std::span(event_data)));
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {2,
           [](SystemCallHandler &handler,
              NativeRuntime &runtime) -> InvokeStatus<void> {
             auto [status, gas] = handler.invoke(
                 handler.get_current_address(), 0, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagNone));
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto [status2, gas2] = handler.invoke(
                 handler.get_current_address(), 1, runtime.get_available_gas(),
                 {}, std::to_underlying(InvokeFlag::kInvokeFlagNone));
             if (!status2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             if (!runtime.consume_gas(gas + gas2)) {
               return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_shared<NativeContext>(std::move(contract_map));

  // mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0xFF);
  write_deployment(contract_0_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  // now invoke the contract

  EphemeralTxIdentifier tx_id;

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {0, 1, 2, 3, 4, 5}},
              }},
      runtime_context, &tx_id);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(*r);

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id);
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().events.size(), 2);
  ASSERT_EQ(tx_event_log.value().events[0].contract_address, contract_0_addr);
  ASSERT_EQ(tx_event_log.value().events[1].contract_address, contract_0_addr);

  auto event_data0 = try_parse_event_data(tx_event_log.value().events[0]);
  ASSERT_TRUE(event_data0.has_value());
  ASSERT_EQ(event_data0.value().topics.size(), 0);
  ASSERT_EQ(event_data0.value().event, make_vec("test 0 topics"));

  auto event_data1 = try_parse_event_data(tx_event_log.value().events[1]);
  ASSERT_TRUE(event_data1.has_value());
  ASSERT_EQ(event_data1.value().topics.size(), 1);
  ASSERT_EQ(event_data1.value().topics[0], sha256(std::string("topic")));
  ASSERT_EQ(event_data1.value().event, make_vec("test 1 topic"));
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
