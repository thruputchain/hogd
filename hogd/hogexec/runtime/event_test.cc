#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "event.hh"
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
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class EventTest : public ::testing::Test, public NativeContractTestFramework {
public:
  EventTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

  void SetUp() override {}

  template<size_t MS = 0xffffffff>
  static x23::Vec<uint8_t, MS> make_vec(std::string_view data)
  {
    return x23::Vec<uint8_t, MS>(reinterpret_cast<uint8_t const *>(data.data()),
                                 reinterpret_cast<uint8_t const *>(
                                     data.data() + data.size()));
  }
  template<size_t MS = 0xffffffff>
  static x23::Vec<uint8_t, MS> make_vec(std::span<const std::byte> data)
  {
    return x23::Vec<uint8_t, MS>(reinterpret_cast<uint8_t const *>(data.data()),
                                 reinterpret_cast<uint8_t const *>(
                                     data.data() + data.size()));
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
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0,
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             const std::string event_data = "test 0 topics";

             auto status = ctx.syscall<SyscallNo::kLogEvent>(
                 static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
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
           [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             const std::string event_data = "test 1 topic";

             SHA256Hash topic = sha256(std::string("topic"));

             auto status = ctx.syscall<SyscallNo::kLogEvent>(
                 &topic, static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 std::as_bytes(std::span(event_data)));
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             return {};
           }},
          {2, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto status = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   ctx.handler().get_current_address(), 0,
                   ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagNone),
                   output_return_buffer_len);
             }();
             [[maybe_unused]] auto gas = GasAmount{0};
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             auto status2 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   ctx.handler().get_current_address(), 1,
                   ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::kInvokeFlagNone),
                   output_return_buffer_len);
             }();
             [[maybe_unused]] auto gas2 = GasAmount{0};
             if (!status2.has_value()) {
               return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
             }

             if (status2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             if (!ctx.runtime().consume_gas(gas + gas2)) {
               return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

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
                                 .calldata = {}},
              }},
      *runtime_context, &tx_id);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id);
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().events.size(), 2);
  ASSERT_EQ(tx_event_log.value().events[0].contract_address, contract_0_addr);
  ASSERT_EQ(tx_event_log.value().events[1].contract_address, contract_0_addr);

  auto event_data0 = tx_event_log.value().events[0];
  ASSERT_EQ(event_data0.num_topics, 0);
  ASSERT_EQ(event_data0.serialized_event,
            make_vec<kMaxEventLength>("test 0 topics"));

  auto event_data1 = tx_event_log.value().events[1];
  ASSERT_EQ(event_data1.num_topics, 1);
  auto topic = sha256(std::string("topic"));
  auto expected_event_data1 =
      make_vec<kMaxEventLength>(std::as_bytes(std::span(topic)));
  expected_event_data1.append_range(make_vec("test 1 topic"));
  ASSERT_EQ(event_data1.serialized_event, expected_event_data1);
}

TEST_F(EventTest, BadEventInputs)
{
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             SHA256Hash topic0 = sha256(std::string("topic"));
             SHA256Hash topic1 = sha256(std::string("topic"));
             SHA256Hash topic2 = sha256(std::string("topic"));

             x23::Vec<uint8_t> too_long_event;
             too_long_event.resize(kMaxEventLength + 1);

             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), nullptr, &topic1, nullptr,
                   std::as_bytes(std::span("test bad topic organization")));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), &topic0, nullptr, &topic2,
                   std::as_bytes(std::span("test bad topic organization")));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), nullptr, nullptr, nullptr,
                   std::as_bytes(std::span(too_long_event)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0xFF);
  write_deployment(contract_0_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  EphemeralTxIdentifier tx_id;

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context, &tx_id);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id);
  ASSERT_FALSE(tx_event_log.has_value());
}

TEST_F(EventTest, EventLengthIncludesTopics)
{
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             SHA256Hash topic = sha256(std::string("topic"));

             x23::Vec<uint8_t> max_event_data;
             max_event_data.resize(kMaxEventLength - topic.size());

             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), &topic, nullptr, nullptr,
                   std::as_bytes(std::span(max_event_data)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             x23::Vec<uint8_t> too_long_event_data;
             too_long_event_data.resize(max_event_data.size() + 1);
             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), &topic, nullptr, nullptr,
                   std::as_bytes(std::span(too_long_event_data)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_0));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0xFF);
  write_deployment(contract_0_addr, contract_0);
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  EphemeralTxIdentifier tx_id;

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context, &tx_id);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id);
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), 1);
  ASSERT_EQ(tx_event_log->events[0].num_topics, 1);
  ASSERT_EQ(tx_event_log->events[0].serialized_event.size(), kMaxEventLength);
}

TEST_F(EventTest, EventWithRevertingFrames)
{
  // A native contract with the following methods:
  // 0: Produces an event with 0 topics, with the event set by the input
  // calldata
  // 1. Calls method 0, and then manually reverts the frame
  // 2. Calls method 0, and then returns a Deterministic error
  // 3. Calls method 0, then method 1, then method 0, then method 2, then method
  // 0.

  // The test part 1 calls method 3; then checks for the events at the end;
  // there should be 3 events. The test part 2 calls method 1; then checks for
  // the events at the end; there should be 0 events (checking manual revert of
  // the top frame)

  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_0 = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            // Method 0: Produces an event with 0 topics, with event data from
            // calldata
            {0,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               // Use calldata as event data
               auto const &calldata = ctx.handler().get_calldata();

               auto status = ctx.syscall<SyscallNo::kLogEvent>(
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   std::as_bytes(std::span(calldata)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               return {};
             }},
            // Method 1: Calls method 0, then manually reverts the frame
            {1,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               // Call method 0 with some test data
               std::string test_data = "method1_event";
               Calldata calldata(test_data.begin(), test_data.end());

               auto status = [&]() {
                 RuntimeMemoryLength output_return_buffer_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     ctx.handler().get_current_address(), 0,
                     ctx.runtime().get_available_gas(), calldata,
                     std::to_underlying(InvokeFlag::kInvokeFlagNone),
                     output_return_buffer_len);
               }();
               [[maybe_unused]] auto gas = GasAmount{0};
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               if (!ctx.runtime().consume_gas(gas)) {
                 return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
               }

               // Manually revert the frame - this should revert the event
               ctx.handler().set_return_buffer(ReturnBuffer{}, true);
               return {};
             }},
            // Method 2: Calls method 0, then returns a Deterministic error
            {2,
             [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               // Call method 0 with some test data
               std::string test_data = "method2_event";
               Calldata calldata(test_data.begin(), test_data.end());

               auto status = [&]() {
                 RuntimeMemoryLength output_return_buffer_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     ctx.handler().get_current_address(), 0,
                     ctx.runtime().get_available_gas(), calldata,
                     std::to_underlying(InvokeFlag::kInvokeFlagNone),
                     output_return_buffer_len);
               }();
               [[maybe_unused]] auto gas = GasAmount{0};
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::kInvokeDeterministic};
               }

               if (!ctx.runtime().consume_gas(gas)) {
                 return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
               }

               // Return a deterministic error - this should revert the event
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }},
            // Method 3: Calls method 0, then 1, then 0, then 2, then 0
            {3, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               // Call method 0 (first event - should succeed)
               {
                 std::string test_data = "first_event";
                 Calldata calldata(test_data.begin(), test_data.end());

                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       ctx.handler().get_current_address(), 0,
                       ctx.runtime().get_available_gas(), calldata,
                       std::to_underlying(InvokeFlag::kInvokeFlagNone),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                 }
               }

               // Call method 1 (should revert, so no event)
               {
                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       ctx.handler().get_current_address(), 1,
                       ctx.runtime().get_available_gas(), Calldata{},
                       std::to_underlying(InvokeFlag::kInvokeFlagNone),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
                 }

                 // Method 1 should have manually reverted
                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeFrameReverted) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                 }
               }

               // Call method 0 again (second event - should succeed)
               {
                 std::string test_data = "second_event";
                 Calldata calldata(test_data.begin(), test_data.end());

                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       ctx.handler().get_current_address(), 0,
                       ctx.runtime().get_available_gas(), calldata,
                       std::to_underlying(InvokeFlag::kInvokeFlagNone),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                 }
               }

               // Call method 2 (should fail with deterministic error, so no
               // event)
               {
                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       ctx.handler().get_current_address(), 2,
                       ctx.runtime().get_available_gas(), Calldata{},
                       std::to_underlying(InvokeFlag::kInvokeFlagNone),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
                 }

                 // Method 2 should have failed with deterministic error
                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                 }
               }

               // Call method 0 one more time (third event - should succeed)
               {
                 std::string test_data = "third_event";
                 Calldata calldata(test_data.begin(), test_data.end());

                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       ctx.handler().get_current_address(), 0,
                       ctx.runtime().get_available_gas(), calldata,
                       std::to_underlying(InvokeFlag::kInvokeFlagNone),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::kInvokeUnrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::kInvokeDeterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::kInvokeOutOfGas};
                 }
               }

               return {};
             }}});

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_0));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  // Mark the contract as existing and deployed within the database
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_0_addr = make_contract_address(0xFF);
  write_deployment(contract_0_addr, contract_0);
  // This test is not focused on storage token costs, so we just grant
  // an arbitrarily large number.
  setup_grant_storage_token(contract_0_addr, 1'000'000'000);

  // Now invoke the contract with method 3
  EphemeralTxIdentifier tx_id_1;

  auto context = make_context();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(3),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *context, &tx_id_1);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events()());

  // Now invoke the contract with method 1
  EphemeralTxIdentifier tx_id_2;
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = contract_0_addr,
                                 .entrypoint = entrypoint_from_method_id(1),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *context, &tx_id_2);

  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());

  auto r2 = handler2.try_commit_transaction();
  ASSERT_EQ(r2, ExecResult::ExecResultTxRootFrameReverted);
  ASSERT_TRUE(handler2.emit_events()());

  advance_block();

  // Check that exactly 3 events were emitted
  auto tx_event_log = get_tx_event_log(tx_id_1);
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().events.size(), 3);

  // Verify the events are from the correct contract
  for (const auto &event : tx_event_log.value().events) {
    ASSERT_EQ(event.contract_address, contract_0_addr);
  }

  // Verify the event data matches what we expect
  auto event_data0 = tx_event_log.value().events[0];
  ASSERT_EQ(event_data0.num_topics, 0);
  ASSERT_EQ(event_data0.serialized_event,
            make_vec<kMaxEventLength>("first_event"));

  auto event_data1 = tx_event_log.value().events[1];
  ASSERT_EQ(event_data1.num_topics, 0);
  ASSERT_EQ(event_data1.serialized_event,
            make_vec<kMaxEventLength>("second_event"));

  auto event_data2 = tx_event_log.value().events[2];
  ASSERT_EQ(event_data2.num_topics, 0);
  ASSERT_EQ(event_data2.serialized_event,
            make_vec<kMaxEventLength>("third_event"));

  auto tx_event_log_2 = get_tx_event_log(tx_id_2);
  ASSERT_FALSE(tx_event_log_2.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
