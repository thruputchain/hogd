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
#include "invoke_flag.hh"
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
  // Emitter: 0 = event with 0 topics, 1 = event with 1 topic.
  // Orchestrator: invokes emitter 0 then emitter 1 (no self-reentrance).
  ContractAddress addr_emitter = make_contract_address(0xFF);
  ContractAddress addr_orchestrator = make_contract_address(0xFE);

  auto native_contract_emitter = std::make_unique<NativeContract>(
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
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }},
          {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             const std::string event_data = "test 1 topic";

             SHA256Hash topic = sha256(std::string("topic"));

             auto status = ctx.syscall<SyscallNo::kLogEvent>(
                 &topic, static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 std::as_bytes(std::span(event_data)));
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  auto native_contract_orchestrator = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [addr_emitter](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto status = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_emitter, 0, ctx.runtime().get_available_gas(),
                   Calldata{}, std::to_underlying(InvokeFlag::None),
                   output_return_buffer_len);
             }();
             [[maybe_unused]] auto gas = GasAmount{0};
             if (!status.has_value()) {
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }

             if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             auto status2 = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_emitter, 1, ctx.runtime().get_available_gas(),
                   Calldata{}, std::to_underlying(InvokeFlag::None),
                   output_return_buffer_len);
             }();
             [[maybe_unused]] auto gas2 = GasAmount{0};
             if (!status2.has_value()) {
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }

             if (status2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             if (!ctx.runtime().consume_gas(gas + gas2)) {
               return InvokeError{InvokeErrorCode::OutOfGas};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_emitter));
  contract_map.emplace(1, std::move(native_contract_orchestrator));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  ContractAddress addrs[] = {addr_emitter, addr_orchestrator};
  for (int i = 0; i < 2; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // now invoke the contract

  EphemeralTxIdentifier tx_id;

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = addr_orchestrator,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context, &tx_id);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().events.size(), 2);
  ASSERT_EQ(tx_event_log.value().events[0].contract_address, addr_emitter);
  ASSERT_EQ(tx_event_log.value().events[1].contract_address, addr_emitter);

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
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }

             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), &topic0, nullptr, &topic2,
                   std::as_bytes(std::span("test bad topic organization")));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }

             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), nullptr, nullptr, nullptr,
                   std::as_bytes(std::span(too_long_event)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::Deterministic};
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
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().exec_result, ExecResult::ExecResultOK);
  ASSERT_TRUE(tx_event_log.value().events.empty());
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
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }

             x23::Vec<uint8_t> too_long_event_data;
             too_long_event_data.resize(max_event_data.size() + 1);
             {
               auto [status, gas] = syscall_LogEvent_inner(
                   ctx.handler(), &topic, nullptr, nullptr,
                   std::as_bytes(std::span(too_long_event_data)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() !=
                   SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
                 return InvokeError{InvokeErrorCode::Deterministic};
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
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), 1);
  ASSERT_EQ(tx_event_log->events[0].num_topics, 1);
  ASSERT_EQ(tx_event_log->events[0].serialized_event.size(), kMaxEventLength);
}

TEST_F(EventTest, EventCountPerTransactionLimit)
{
  auto native_contract_0 = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             const std::string event_data = "event-count-limit";

             for (uint32_t i = 0; i < kMaxEventsPerTransaction; ++i) {
               auto status = ctx.syscall<SyscallNo::kLogEvent>(
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   std::as_bytes(std::span(event_data)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }
               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }

             auto overflow_status = ctx.syscall<SyscallNo::kLogEvent>(
                 static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 std::as_bytes(std::span(event_data)));
             if (!overflow_status.has_value()) {
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }
             if (overflow_status.value() !=
                 SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               return InvokeError{InvokeErrorCode::Deterministic};
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
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), kMaxEventsPerTransaction);
}

TEST_F(EventTest, EventCountPerTransactionLimitIncludesChildFrames)
{
  constexpr uint32_t kParentEventCount = 600;
  constexpr uint32_t kChildEventCount =
      kMaxEventsPerTransaction - kParentEventCount;

  ContractAddress addr_child = make_contract_address(0xFF);
  ContractAddress addr_parent = make_contract_address(0xFE);

  auto native_contract_child = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             const std::string event_data = "child-event-count-limit";

             for (uint32_t i = 0; i < kChildEventCount; ++i) {
               auto status = ctx.syscall<SyscallNo::kLogEvent>(
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   std::as_bytes(std::span(event_data)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }
               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }

             auto overflow_status = ctx.syscall<SyscallNo::kLogEvent>(
                 static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 static_cast<SHA256Hash const *>(nullptr),
                 std::as_bytes(std::span(event_data)));
             if (!overflow_status.has_value()) {
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }
             if (overflow_status.value() !=
                 SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  auto native_contract_parent = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [addr_child](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             const std::string event_data = "parent-event-count-limit";

             for (uint32_t i = 0; i < kParentEventCount; ++i) {
               auto status = ctx.syscall<SyscallNo::kLogEvent>(
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   std::as_bytes(std::span(event_data)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }
               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }
             }

             auto invoke_status = [&]() {
               RuntimeMemoryLength output_return_buffer_len = 0;
               return ctx.syscall<SyscallNo::kInvoke>(
                   addr_child, 0, ctx.runtime().get_available_gas(), Calldata{},
                   std::to_underlying(InvokeFlag::None),
                   output_return_buffer_len);
             }();
             if (!invoke_status.has_value()) {
               return InvokeError{InvokeErrorCode::Unrecoverable};
             }
             if (invoke_status.value() !=
                 SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract_child));
  contract_map.emplace(1, std::move(native_contract_parent));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  auto child_contract = make_native_contract(0);
  auto parent_contract = make_native_contract(1);
  write_contract(child_contract);
  write_contract(parent_contract);
  write_deployment(addr_child, child_contract);
  write_deployment(addr_parent, parent_contract);
  setup_grant_storage_token(addr_child, 1'000'000'000);
  setup_grant_storage_token(addr_parent, 1'000'000'000);

  EphemeralTxIdentifier tx_id;

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = addr_parent,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *runtime_context, &tx_id);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), kMaxEventsPerTransaction);
}

TEST_F(EventTest, EventWithRevertingFrames)
{
  // Four contracts:
  // - Emitter: logs 0-topic event from calldata.
  // - ManualRevert: invokes emitter with fixed calldata, then manual frame
  // revert.
  // - DeterministicFail: invokes emitter with fixed calldata, then
  // Deterministic error.
  // - Orchestrator: emitter, manual-revert, emitter, deterministic-fail,
  // emitter.
  //
  // Part 1 invokes orchestrator:0; expect 3 committed events. Part 2 invokes
  // manual-revert:0; expect 0 events (top-frame manual revert).

  ContractAddress addr_emitter = make_contract_address(0xFF);
  ContractAddress addr_manual_revert = make_contract_address(0xFE);
  ContractAddress addr_deterministic = make_contract_address(0xFD);
  ContractAddress addr_orchestrator = make_contract_address(0xFC);

  auto make_context =
      [addr_emitter, addr_manual_revert,
       addr_deterministic]() -> std::unique_ptr<RuntimeContext> {
    auto native_contract_emitter = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               auto const &calldata = ctx.handler().get_calldata();

               auto status = ctx.syscall<SyscallNo::kLogEvent>(
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   static_cast<SHA256Hash const *>(nullptr),
                   std::as_bytes(std::span(calldata)));
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }

               return {};
             }}});

    auto native_contract_manual_revert = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [addr_emitter](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               std::string test_data = "method1_event";
               Calldata calldata(test_data.begin(), test_data.end());

               auto status = [&]() {
                 RuntimeMemoryLength output_return_buffer_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     addr_emitter, 0, ctx.runtime().get_available_gas(),
                     calldata, std::to_underlying(InvokeFlag::None),
                     output_return_buffer_len);
               }();
               [[maybe_unused]] auto gas = GasAmount{0};
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }

               if (!ctx.runtime().consume_gas(gas)) {
                 return InvokeError{InvokeErrorCode::OutOfGas};
               }

               ctx.handler().set_return_buffer(ReturnBuffer{}, true);
               return {};
             }}});

    auto native_contract_deterministic = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [addr_emitter](NativeSyscallContext &ctx) -> InvokeStatus<void> {
               std::string test_data = "method2_event";
               Calldata calldata(test_data.begin(), test_data.end());

               auto status = [&]() {
                 RuntimeMemoryLength output_return_buffer_len = 0;
                 return ctx.syscall<SyscallNo::kInvoke>(
                     addr_emitter, 0, ctx.runtime().get_available_gas(),
                     calldata, std::to_underlying(InvokeFlag::None),
                     output_return_buffer_len);
               }();
               [[maybe_unused]] auto gas = GasAmount{0};
               if (!status.has_value()) {
                 return InvokeError{InvokeErrorCode::Unrecoverable};
               }

               if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
                 return InvokeError{InvokeErrorCode::Deterministic};
               }

               if (!ctx.runtime().consume_gas(gas)) {
                 return InvokeError{InvokeErrorCode::OutOfGas};
               }

               return InvokeError{InvokeErrorCode::Deterministic};
             }}});

    auto native_contract_orchestrator = std::make_unique<NativeContract>(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
            {0,
             [addr_emitter, addr_manual_revert, addr_deterministic](
                 NativeSyscallContext &ctx) -> InvokeStatus<void> {
               {
                 std::string test_data = "first_event";
                 Calldata calldata(test_data.begin(), test_data.end());

                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       addr_emitter, 0, ctx.runtime().get_available_gas(),
                       calldata, std::to_underlying(InvokeFlag::None),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::Unrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::OutOfGas};
                 }
               }

               {
                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       addr_manual_revert, 0, ctx.runtime().get_available_gas(),
                       Calldata{}, std::to_underlying(InvokeFlag::None),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::Unrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeFrameReverted) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::OutOfGas};
                 }
               }

               {
                 std::string test_data = "second_event";
                 Calldata calldata(test_data.begin(), test_data.end());

                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       addr_emitter, 0, ctx.runtime().get_available_gas(),
                       calldata, std::to_underlying(InvokeFlag::None),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::Unrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::OutOfGas};
                 }
               }

               {
                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       addr_deterministic, 0, ctx.runtime().get_available_gas(),
                       Calldata{}, std::to_underlying(InvokeFlag::None),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::Unrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeDeterministicWasm) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::OutOfGas};
                 }
               }

               {
                 std::string test_data = "third_event";
                 Calldata calldata(test_data.begin(), test_data.end());

                 auto status = [&]() {
                   RuntimeMemoryLength output_return_buffer_len = 0;
                   return ctx.syscall<SyscallNo::kInvoke>(
                       addr_emitter, 0, ctx.runtime().get_available_gas(),
                       calldata, std::to_underlying(InvokeFlag::None),
                       output_return_buffer_len);
                 }();
                 [[maybe_unused]] auto gas = GasAmount{0};
                 if (!status.has_value()) {
                   return InvokeError{InvokeErrorCode::Unrecoverable};
                 }

                 if (status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
                   return InvokeError{InvokeErrorCode::Deterministic};
                 }

                 if (!ctx.runtime().consume_gas(gas)) {
                   return InvokeError{InvokeErrorCode::OutOfGas};
                 }
               }

               return {};
             }}});

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract_emitter));
    contract_map.emplace(1, std::move(native_contract_manual_revert));
    contract_map.emplace(2, std::move(native_contract_deterministic));
    contract_map.emplace(3, std::move(native_contract_orchestrator));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  ContractAddress addrs[] = {addr_emitter, addr_manual_revert,
                             addr_deterministic, addr_orchestrator};
  for (int i = 0; i < 4; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  // Part 1: orchestrator sequence
  EphemeralTxIdentifier tx_id_1;

  auto context = make_context();

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = addr_orchestrator,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *context, &tx_id_1);

  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());

  // Part 2: manual-revert entry alone
  EphemeralTxIdentifier tx_id_2;
  auto handler2 = make_new_system_call_handler(
      SignedTransaction{
          .tx =
              Transaction{
                  .invocation =
                      Invocation{.invoked_address = addr_manual_revert,
                                 .entrypoint = entrypoint_from_method_id(0),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
              }},
      *context, &tx_id_2);

  auto res2 = handler2.root_invoke();
  ASSERT_TRUE(res2.is_root_success());

  auto r2 = handler2.try_commit_transaction();
  ASSERT_EQ(r2, ExecResult::ExecResultTxRootFrameReverted);
  ASSERT_TRUE(
      handler2.emit_events(ExecResult::ExecResultTxRootFrameReverted)());

  advance_block();

  // Check that exactly 3 events were emitted
  auto tx_event_log = get_tx_event_log(tx_id_1)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().events.size(), 3);

  // Verify the events are from the emitter contract
  for (const auto &event : tx_event_log.value().events) {
    ASSERT_EQ(event.contract_address, addr_emitter);
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

  auto tx_event_log_2 = get_tx_event_log(tx_id_2)();
  ASSERT_TRUE(tx_event_log_2.has_value());
  ASSERT_EQ(tx_event_log_2.value().exec_result,
            ExecResult::ExecResultTxRootFrameReverted);
  ASSERT_TRUE(tx_event_log_2.value().events.empty());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
