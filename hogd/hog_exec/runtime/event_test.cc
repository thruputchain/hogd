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
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/cpumap.h"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class EventTest : public ::testing::Test, public NativeContractTestFramework {
public:
  EventTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
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

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_emitter;
  fns_emitter.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "test 0 topics";

        auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            std::as_bytes(std::span(event_data)));
        if (!status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }

        if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  fns_emitter.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "test 1 topic";

        SHA256Hash topic = sha256(std::string("topic"));

        auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
            &topic, static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            std::as_bytes(std::span(event_data)));
        if (!status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }

        if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_emitter =
      std::make_unique<NativeContract>(std::move(fns_emitter));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_orchestrator;
  fns_orchestrator.emplace(
      0,
      [addr_emitter](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto status =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              addr_emitter, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, CopyableInvokeOutputs{});
        }();
        [[maybe_unused]] auto gas = GasAmount{0};
        if (!status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }

        if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto status2 =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 1,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              addr_emitter, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, CopyableInvokeOutputs{});
        }();
        [[maybe_unused]] auto gas2 = GasAmount{0};
        if (!status2.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }

        if (status2.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        if (!ctx.consume_gas(gas + gas2)) {
          co_return InvokeError{InvokeErrorCode::OutOfGas};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_orchestrator =
      std::make_unique<NativeContract>(std::move(fns_orchestrator));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_orchestrator,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

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
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        SHA256Hash topic0 = sha256(std::string("topic"));
        SHA256Hash topic1 = sha256(std::string("topic"));
        SHA256Hash topic2 = sha256(std::string("topic"));

        x23::Vec<uint8_t> too_long_event;
        too_long_event.resize(kMaxEventLength + 1);

        {
          auto [status, gas] = co_await syscall_LogEvent_inner(
              ctx.handler(), nullptr, &topic1, nullptr,
              std::as_bytes(std::span("test bad topic organization")));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        {
          auto [status, gas] = co_await syscall_LogEvent_inner(
              ctx.handler(), &topic0, nullptr, &topic2,
              std::as_bytes(std::span("test bad topic organization")));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        {
          auto [status, gas] = co_await syscall_LogEvent_inner(
              ctx.handler(), nullptr, nullptr, nullptr,
              std::as_bytes(std::span(too_long_event)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = contract_0_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log.value().exec_result, ExecResult::ExecResultOK);
  ASSERT_TRUE(tx_event_log.value().events.empty());
}

TEST_F(EventTest, EventLengthIncludesTopics)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        SHA256Hash topic = sha256(std::string("topic"));

        x23::Vec<uint8_t> max_event_data;
        max_event_data.resize(kMaxEventLength - topic.size());

        {
          auto [status, gas] = co_await syscall_LogEvent_inner(
              ctx.handler(), &topic, nullptr, nullptr,
              std::as_bytes(std::span(max_event_data)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        x23::Vec<uint8_t> too_long_event_data;
        too_long_event_data.resize(max_event_data.size() + 1);
        {
          auto [status, gas] = co_await syscall_LogEvent_inner(
              ctx.handler(), &topic, nullptr, nullptr,
              std::as_bytes(std::span(too_long_event_data)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() !=
              SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = contract_0_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), 1);
  ASSERT_EQ(tx_event_log->events[0].num_topics, 1);
  ASSERT_EQ(tx_event_log->events[0].serialized_event.size(), kMaxEventLength);
}

TEST_F(EventTest, EventCountPerTransactionLimit)
{
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_0;
  fns_0.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "event-count-limit";

        for (uint32_t i = 0; i < kMaxEventsPerTransaction; ++i) {
          auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              std::as_bytes(std::span(event_data)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        auto overflow_status = co_await ctx.syscall<SyscallNo::kLogEvent>(
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            std::as_bytes(std::span(event_data)));
        if (!overflow_status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        if (overflow_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_0 = std::make_unique<NativeContract>(std::move(fns_0));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = contract_0_addr,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

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

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "child-event-count-limit";

        for (uint32_t i = 0; i < kChildEventCount; ++i) {
          auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              std::as_bytes(std::span(event_data)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        auto overflow_status = co_await ctx.syscall<SyscallNo::kLogEvent>(
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            std::as_bytes(std::span(event_data)));
        if (!overflow_status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        if (overflow_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_child =
      std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [addr_child](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "parent-event-count-limit";

        for (uint32_t i = 0; i < kParentEventCount; ++i) {
          auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              std::as_bytes(std::span(event_data)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }

        auto invoke_status =
            co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          co_return co_await ctx.syscall<SyscallNo::kInvoke>(
              addr_child, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, CopyableInvokeOutputs{});
        }();
        if (!invoke_status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        if (invoke_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });
  auto native_contract_parent =
      std::make_unique<NativeContract>(std::move(fns_parent));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_parent,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), kMaxEventsPerTransaction);
}

// The per-transaction event cap bounds the *committed* event log:
// TxEventLog::events is an XDR vector whose declared maximum length is
// kMaxEventsPerTransaction. A manual frame revert discards everything the
// frame emitted, so InvocationFrame::commit_manually_reverted_frame
// deliberately does not propagate the child's event count back to the parent
// -- events that can never reach the log must not consume log capacity.
// InvocationFrame::commit_failed_child_frame omits the same propagation, for
// the same reason.
//
// The visible consequence, pinned by this test, is that each manually
// reverted child restarts from its parent's unchanged count and can admit a
// full kMaxEventsPerTransaction worth of host-side event materializations.
// That is working as intended (#3520): repeating that work costs the caller a
// child frame plus kMaxEventsPerTransaction syscalls each time, so it is
// bounded by gas and by the frame/instruction budget rather than by the
// log-capacity counter.
//
// Do NOT "fix" this by propagating event_count_ through the manual-revert or
// failed-child commit paths. Doing so would let an aborted or faulting
// subcall permanently burn committed-log capacity it never used, which would
// be a correctness regression, not a hardening.
TEST_F(EventTest, ManuallyRevertedChildFramesEachGetFullEventAllowance)
{
  constexpr uint32_t kRevertingChildCalls = 3;

  ContractAddress addr_child = make_contract_address(0xFF);
  ContractAddress addr_parent = make_contract_address(0xFE);

  uint32_t accepted_event_materializations = 0;
  InvocationFrameStatusCode frame_statuses[kRevertingChildCalls]{};

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_child;
  fns_child.emplace(
      0,
      [&accepted_event_materializations](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "reverted-child-event";

        for (uint32_t i = 0; i < kMaxEventsPerTransaction; ++i) {
          auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              std::as_bytes(std::span(event_data)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          ++accepted_event_materializations;
        }

        // The cap still binds *within* a reverting frame: a child gets one
        // allowance at a time, never two.
        auto overflow_status = co_await ctx.syscall<SyscallNo::kLogEvent>(
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            std::as_bytes(std::span(event_data)));
        if (!overflow_status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        if (overflow_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, true, std::span<const std::byte>{});
        if (ret.has_value() || ret.error().code() != HostFnErrorCode::Return) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        co_return InvokeStatus<void>{};
      });
  auto native_contract_child =
      std::make_unique<NativeContract>(std::move(fns_child));

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [addr_child, &frame_statuses](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (auto &frame_status : frame_statuses) {
          CopyableInvokeOutputs outputs{};
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto status = co_await ctx.syscall<SyscallNo::kInvoke>(
              addr_child, std::span<const std::byte>{}, std::span<uint32_t>{},
              inputs, outputs);
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          // A child frame ran, so Invoke reports None; the manual revert
          // surfaces as frame_status == Abort.
          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          frame_status = outputs.frame_status;
        }
        co_return InvokeStatus<void>{};
      });
  auto native_contract_parent =
      std::make_unique<NativeContract>(std::move(fns_parent));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_parent,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  // Every reverted child saw a fresh, full allowance.
  ASSERT_EQ(accepted_event_materializations,
            kRevertingChildCalls * kMaxEventsPerTransaction);
  for (auto frame_status : frame_statuses) {
    ASSERT_EQ(frame_status, InvocationFrameStatusCode::Abort);
  }

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  // ...and none of that work reached the committed log, which is what the cap
  // actually protects.
  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_TRUE(tx_event_log->events.empty());
}

// Companion to the test above (#3520). Because the cap is enforced against
// the committed log, events discarded by a manually reverted child -- or by a
// child that faults -- leave the parent's allowance fully intact: the parent
// still commits exactly kMaxEventsPerTransaction events afterwards and is
// refused at the next one. Contrast EventCountPerTransactionLimit-
// IncludesChildFrames, where a *successful* child's events do commit and so
// do consume the parent's allowance.
TEST_F(EventTest, DiscardedChildEventsDoNotConsumeCommittedEventAllowance)
{
  ContractAddress addr_revert_child = make_contract_address(0xFF);
  ContractAddress addr_fault_child = make_contract_address(0xFE);
  ContractAddress addr_parent = make_contract_address(0xFD);

  uint32_t accepted_event_materializations = 0;
  InvocationFrameStatusCode revert_child_status{};
  InvocationFrameStatusCode fault_child_status{};

  auto fill_allowance = [](NativeSyscallContext &ctx,
                           std::string const &event_data,
                           uint32_t &accepted) -> x23::Async<bool> {
    for (uint32_t i = 0; i < kMaxEventsPerTransaction; ++i) {
      auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
          static_cast<SHA256Hash const *>(nullptr),
          static_cast<SHA256Hash const *>(nullptr),
          static_cast<SHA256Hash const *>(nullptr),
          std::as_bytes(std::span(event_data)));
      if (!status.has_value() ||
          status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
        co_return false;
      }
      ++accepted;
    }
    co_return true;
  };

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_revert_child;
  fns_revert_child.emplace(
      0,
      [&accepted_event_materializations, fill_allowance](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "reverted-child";
        if (!co_await fill_allowance(ctx, event_data,
                                     accepted_event_materializations)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto ret = co_await ctx.syscall<SyscallNo::kReturn>(
            std::span<const std::byte>{}, true, std::span<const std::byte>{});
        if (ret.has_value() || ret.error().code() != HostFnErrorCode::Return) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<Runtime::MethodID, NativeContract::function_t>
      fns_fault_child;
  fns_fault_child.emplace(
      0,
      [&accepted_event_materializations, fill_allowance](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        const std::string event_data = "faulted-child";
        if (!co_await fill_allowance(ctx, event_data,
                                     accepted_event_materializations)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        // Fault after the emissions: commit_failed_child_frame drops them.
        co_return InvokeError{InvokeErrorCode::Deterministic};
      });

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns_parent;
  fns_parent.emplace(
      0,
      [addr_revert_child, addr_fault_child, &accepted_event_materializations,
       &revert_child_status, &fault_child_status, fill_allowance](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto invoke_child =
            [&ctx](ContractAddress const &addr,
                   InvocationFrameStatusCode &status_out) -> x23::Async<bool> {
          CopyableInvokeOutputs outputs{};
          CopyableInvokeInputs inputs{
              .entrypoint_id = 0,
              .gas_limit = UINT64_MAX,
              .flags = std::to_underlying(InvokeFlag::None),
          };
          auto status = co_await ctx.syscall<SyscallNo::kInvoke>(
              addr, std::span<const std::byte>{}, std::span<uint32_t>{}, inputs,
              outputs);
          if (!status.has_value() ||
              status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return false;
          }
          status_out = outputs.frame_status;
          co_return true;
        };

        if (!co_await invoke_child(addr_revert_child, revert_child_status)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        if (!co_await invoke_child(addr_fault_child, fault_child_status)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // The parent's own allowance is untouched by either discarded child.
        const std::string event_data = "parent-committed";
        if (!co_await fill_allowance(ctx, event_data,
                                     accepted_event_materializations)) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto overflow_status = co_await ctx.syscall<SyscallNo::kLogEvent>(
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            static_cast<SHA256Hash const *>(nullptr),
            std::as_bytes(std::span(event_data)));
        if (!overflow_status.has_value()) {
          co_return InvokeError{InvokeErrorCode::Unrecoverable};
        }
        if (overflow_status.value() !=
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::make_unique<NativeContract>(
                              std::move(fns_revert_child)));
  contract_map.emplace(1, std::make_unique<NativeContract>(
                              std::move(fns_fault_child)));
  contract_map.emplace(2,
                       std::make_unique<NativeContract>(std::move(fns_parent)));
  auto runtime_context =
      std::make_unique<NativeContext>(std::move(contract_map));

  ContractAddress addrs[] = {addr_revert_child, addr_fault_child, addr_parent};
  for (int i = 0; i < 3; ++i) {
    auto c = make_native_contract(i);
    write_contract(c);
    write_deployment(addrs[i], c);
    setup_grant_storage_token(addrs[i], 1'000'000'000);
  }

  EphemeralTxIdentifier tx_id;

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_parent,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler =
      make_new_system_call_handler(handler_tx, *runtime_context, &tx_id);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  ASSERT_EQ(revert_child_status, InvocationFrameStatusCode::Abort);
  ASSERT_EQ(fault_child_status, InvocationFrameStatusCode::Fault);
  // Three full allowances were materialized host-side; only the parent's
  // survives into the log.
  ASSERT_EQ(accepted_event_materializations, 3 * kMaxEventsPerTransaction);

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  advance_block();

  auto tx_event_log = get_tx_event_log(tx_id)();
  ASSERT_TRUE(tx_event_log.has_value());
  ASSERT_EQ(tx_event_log->events.size(), kMaxEventsPerTransaction);
  for (auto const &event : tx_event_log->events) {
    ASSERT_EQ(event.contract_address, addr_parent);
    ASSERT_EQ(event.num_topics, 0);
    ASSERT_EQ(event.serialized_event,
              make_vec<kMaxEventLength>("parent-committed"));
  }
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
    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_emitter;
    fns_emitter.emplace(
        0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          auto const &calldata = ctx.handler().get_calldata();

          auto status = co_await ctx.syscall<SyscallNo::kLogEvent>(
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              static_cast<SHA256Hash const *>(nullptr),
              std::as_bytes(std::span(calldata)));
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_emitter =
        std::make_unique<NativeContract>(std::move(fns_emitter));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_manual_revert;
    fns_manual_revert.emplace(
        0,
        [addr_emitter](
            NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          std::string test_data = "method1_event";
          Calldata calldata(test_data.begin(), test_data.end());

          auto status =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                addr_emitter, calldata, std::span<uint32_t>{}, inputs,
                CopyableInvokeOutputs{});
          }();
          [[maybe_unused]] auto gas = GasAmount{0};
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          if (!ctx.consume_gas(gas)) {
            co_return InvokeError{InvokeErrorCode::OutOfGas};
          }

          ctx.handler().set_return_buffer(ReturnBuffer{}, true);
          co_return InvokeStatus<void>{};
        });
    auto native_contract_manual_revert =
        std::make_unique<NativeContract>(std::move(fns_manual_revert));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_deterministic;
    fns_deterministic.emplace(
        0,
        [addr_emitter](
            NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          std::string test_data = "method2_event";
          Calldata calldata(test_data.begin(), test_data.end());

          auto status =
              co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
            CopyableInvokeInputs inputs{
                .entrypoint_id = 0,
                .gas_limit = UINT64_MAX,
                .flags = std::to_underlying(InvokeFlag::None),
            };
            co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                addr_emitter, calldata, std::span<uint32_t>{}, inputs,
                CopyableInvokeOutputs{});
          }();
          [[maybe_unused]] auto gas = GasAmount{0};
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }

          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          if (!ctx.consume_gas(gas)) {
            co_return InvokeError{InvokeErrorCode::OutOfGas};
          }

          co_return InvokeError{InvokeErrorCode::Deterministic};
        });
    auto native_contract_deterministic =
        std::make_unique<NativeContract>(std::move(fns_deterministic));

    std::unordered_map<Runtime::MethodID, NativeContract::function_t>
        fns_orchestrator;
    fns_orchestrator.emplace(
        0,
        [addr_emitter, addr_manual_revert, addr_deterministic](
            NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          {
            std::string test_data = "first_event";
            Calldata calldata(test_data.begin(), test_data.end());

            auto status =
                co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
              CopyableInvokeInputs inputs{
                  .entrypoint_id = 0,
                  .gas_limit = UINT64_MAX,
                  .flags = std::to_underlying(InvokeFlag::None),
              };
              co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                  addr_emitter, calldata, std::span<uint32_t>{}, inputs,
                  CopyableInvokeOutputs{});
            }();
            [[maybe_unused]] auto gas = GasAmount{0};
            if (!status.has_value()) {
              co_return InvokeError{InvokeErrorCode::Unrecoverable};
            }

            if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (!ctx.consume_gas(gas)) {
              co_return InvokeError{InvokeErrorCode::OutOfGas};
            }
          }

          {
            CopyableInvokeOutputs status_outputs{};
            auto status =
                co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
              CopyableInvokeInputs inputs{
                  .entrypoint_id = 0,
                  .gas_limit = UINT64_MAX,
                  .flags = std::to_underlying(InvokeFlag::None),
              };
              co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                  addr_manual_revert, std::span<const std::byte>{},
                  std::span<uint32_t>{}, inputs, status_outputs);
            }();
            [[maybe_unused]] auto gas = GasAmount{0};
            if (!status.has_value()) {
              co_return InvokeError{InvokeErrorCode::Unrecoverable};
            }

            // A child frame ran, so Invoke returns None; the manual revert is
            // reported via frame_status == Abort.
            if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (status_outputs.frame_status !=
                InvocationFrameStatusCode::Abort) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (!ctx.consume_gas(gas)) {
              co_return InvokeError{InvokeErrorCode::OutOfGas};
            }
          }

          {
            std::string test_data = "second_event";
            Calldata calldata(test_data.begin(), test_data.end());

            auto status =
                co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
              CopyableInvokeInputs inputs{
                  .entrypoint_id = 0,
                  .gas_limit = UINT64_MAX,
                  .flags = std::to_underlying(InvokeFlag::None),
              };
              co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                  addr_emitter, calldata, std::span<uint32_t>{}, inputs,
                  CopyableInvokeOutputs{});
            }();
            [[maybe_unused]] auto gas = GasAmount{0};
            if (!status.has_value()) {
              co_return InvokeError{InvokeErrorCode::Unrecoverable};
            }

            if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (!ctx.consume_gas(gas)) {
              co_return InvokeError{InvokeErrorCode::OutOfGas};
            }
          }

          {
            CopyableInvokeOutputs status_outputs{};
            auto status =
                co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
              CopyableInvokeInputs inputs{
                  .entrypoint_id = 0,
                  .gas_limit = UINT64_MAX,
                  .flags = std::to_underlying(InvokeFlag::None),
              };
              co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                  addr_deterministic, std::span<const std::byte>{},
                  std::span<uint32_t>{}, inputs, status_outputs);
            }();
            [[maybe_unused]] auto gas = GasAmount{0};
            if (!status.has_value()) {
              co_return InvokeError{InvokeErrorCode::Unrecoverable};
            }

            // A child frame ran, so Invoke returns None; the trap / out-of-gas
            // is reported via frame_status == Fault.
            if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (status_outputs.frame_status !=
                InvocationFrameStatusCode::Fault) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (!ctx.consume_gas(gas)) {
              co_return InvokeError{InvokeErrorCode::OutOfGas};
            }
          }

          {
            std::string test_data = "third_event";
            Calldata calldata(test_data.begin(), test_data.end());

            auto status =
                co_await [&]() -> x23::Async<HostFnStatus<SyscallErrorCode>> {
              CopyableInvokeInputs inputs{
                  .entrypoint_id = 0,
                  .gas_limit = UINT64_MAX,
                  .flags = std::to_underlying(InvokeFlag::None),
              };
              co_return co_await ctx.syscall<SyscallNo::kInvoke>(
                  addr_emitter, calldata, std::span<uint32_t>{}, inputs,
                  CopyableInvokeOutputs{});
            }();
            [[maybe_unused]] auto gas = GasAmount{0};
            if (!status.has_value()) {
              co_return InvokeError{InvokeErrorCode::Unrecoverable};
            }

            if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
              co_return InvokeError{InvokeErrorCode::Deterministic};
            }

            if (!ctx.consume_gas(gas)) {
              co_return InvokeError{InvokeErrorCode::OutOfGas};
            }
          }

          co_return InvokeStatus<void>{};
        });
    auto native_contract_orchestrator =
        std::make_unique<NativeContract>(std::move(fns_orchestrator));

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

  auto handler_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_orchestrator,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler = make_new_system_call_handler(handler_tx, *context, &tx_id_1);

  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());

  auto r = handler.try_commit_transaction()();
  ASSERT_EQ(r, ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = handler_tx})());

  // Part 2: manual-revert entry alone
  EphemeralTxIdentifier tx_id_2;
  auto handler2_tx = SignedTransaction{
      .tx = Transaction{
          .invocation = Invocation{.invoked_address = addr_manual_revert,
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
          .uniquifier = make_new_uniquifier(),
      }};
  auto handler2 = make_new_system_call_handler(handler2_tx, *context, &tx_id_2);

  auto res2 = handler2.root_invoke()();
  ASSERT_TRUE(res2.is_root_success());

  auto r2 = handler2.try_commit_transaction()();
  ASSERT_EQ(r2, ExecResult::ExecResultTxRootFrameReverted);
  ASSERT_TRUE(
      handler2.emit_events(ExecResult::ExecResultTxRootFrameReverted,
                           ProposedTransaction{.signed_tx = handler2_tx})());

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

// Regression test for #2857: a validation-mode tx with enough consumed
// RCQ-observation proposal data to overflow the event RPC once forwarded (the
// cap is lowered so 16 observations suffice). Before the fix this threw
// uncaught and terminated the executor; now emit_events fail-stops with a
// structured Unrecoverable status.
namespace {

constexpr size_t kValidationProposalObservationCount = 16;
constexpr size_t kValidationProposalEventRpcCap = 4096;
constexpr ContractAddress kValidationProposalEventRpcAddress =
    NativeContractTestFramework::make_contract_address(0xE2);

StorageKeyContractData
make_validation_proposal_observation_key(size_t index)
{
  auto key = NativeContractTestFramework::make_storage_key("event_rpc_poc");
  key.back() = static_cast<uint8_t>(index);
  return key;
}

Transaction
make_validation_proposal_event_rpc_tx(NativeContractTestFramework &fw,
                                      Runtime::MethodID method_id)
{
  return Transaction{
      .invocation =
          Invocation{.invoked_address = kValidationProposalEventRpcAddress,
                     .entrypoint = entrypoint_from_method_id(method_id),
                     .gas_limit = UINT64_MAX,
                     .calldata = {}},
      .uniquifier = fw.make_new_uniquifier(),
  };
}

} // namespace

TEST(EventRpcValidationProposalData,
     OversizedConsumedProposalDataFailStopsInsteadOfCrashing)
{
  NativeContractTestFramework fw(
      false, BlockExecutionMode::kBlockExecutionModeProposal, TestDB::kHogDb);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                for (size_t i = 0; i < kValidationProposalObservationCount;
                     ++i) {
                  QueueEntry entry;
                  entry.resize(kQueueEntryMaxLen, static_cast<uint8_t>(i));
                  auto status = co_await ctx.handler().retrocausal_queue_insert(
                      make_validation_proposal_observation_key(i),
                      std::as_bytes(std::span(entry.data(), entry.size())));
                  if (!status.has_value()) {
                    co_return InvokeError{InvokeErrorCode::Unrecoverable};
                  }
                }
                co_return InvokeStatus<void>{};
              });
  fns.emplace(
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        for (size_t i = 0; i < kValidationProposalObservationCount; ++i) {
          RuntimeMemoryLength len = 0;
          auto status =
              co_await ctx.syscall<SyscallNo::kRetrocausalQueuePeekLen>(
                  make_validation_proposal_observation_key(i), len);
          if (!status.has_value()) {
            co_return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (status.value() != SyscallErrorCode::kSyscallErrorCodeNone ||
              len != kQueueEntryMaxLen) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
        }
        co_return InvokeStatus<void>{};
      });

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = NativeContractTestFramework::make_native_contract(0);
  fw.write_contract(contract);
  fw.write_deployment(kValidationProposalEventRpcAddress, contract);
  fw.setup_grant_storage_token(kValidationProposalEventRpcAddress,
                               1'000'000'000);

  auto insert_tx = make_validation_proposal_event_rpc_tx(fw, 0);
  auto consume_tx = make_validation_proposal_event_rpc_tx(fw, 1);

  NondeterministicProposalData insert_proposal_data;
  NondeterministicProposalData consume_proposal_data;
  {
    auto insert_handler = fw.make_new_system_call_handler(insert_tx, *context);
    ASSERT_TRUE(insert_handler.root_invoke()().is_root_success());
    insert_proposal_data = insert_handler.get_proposal_data();
    ASSERT_EQ(insert_handler.try_commit_transaction()(),
              ExecResult::ExecResultOK);

    auto consume_handler =
        fw.make_new_system_call_handler(consume_tx, *context);
    ASSERT_TRUE(consume_handler.root_invoke()().is_root_success());
    consume_proposal_data = consume_handler.get_proposal_data();
    ASSERT_EQ(consume_proposal_data.nondeterministic_bit_count,
              kValidationProposalObservationCount);
    ASSERT_EQ(consume_proposal_data.observed_queue_entry_sequence.size(),
              kValidationProposalObservationCount);
    ASSERT_EQ(consume_handler.try_commit_transaction()(),
              ExecResult::ExecResultOK);
  }

  fw.discard_block(BlockExecutionMode::kBlockExecutionModeValidation);

  {
    auto insert_handler =
        fw.make_new_system_call_handler_validation(insert_tx, *context,
                                                   insert_proposal_data);
    ASSERT_TRUE(insert_handler.root_invoke()().is_root_success());
    ASSERT_EQ(insert_handler.try_commit_transaction()(),
              ExecResult::ExecResultOK);
  }

  // Cap only this proxy's client streams. Mutating the process-wide
  // x23::default_max_message_size here would race with the event server's
  // accept threads, which read it while constructing their side of these
  // very connections.
  auto capped_event_proxy = std::make_unique<EventServerProxy>(
      x23::HostPort{"localhost", fw.get_event_exec_port()}, *x23::llc_map,
      kValidationProposalEventRpcCap);

  SystemCallHandler consume_validation_handler(
      consume_tx, *fw.get_storage_snapshot_cache(),
      fw.make_new_per_tx_db_proxy(1001),
      PerTransactionEventServerProxy(*capped_event_proxy, fw.get_block_number(),
                                     1001),
      ValidationModeCtor{consume_proposal_data}, *context);
  ASSERT_TRUE(consume_validation_handler.root_invoke()().is_root_success());
  ASSERT_EQ(consume_validation_handler.try_commit_transaction()(),
            ExecResult::ExecResultOK);

  ProposedTransaction proposed_tx{
      .signed_tx = SignedTransaction{.tx = consume_tx},
      .nondeterministic_proposal_data = std::move(consume_proposal_data),
  };
  // With the emit_event fail-stop backstop (#2857), the oversized payload no
  // longer escapes as an uncaught x23::XdrBadInput that would terminate the
  // executor; emit_events completes with a structured Unrecoverable status.
  auto status =
      consume_validation_handler.emit_events(ExecResult::ExecResultOK,
                                             std::move(proposed_tx))();
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), SystemErrorCode::Unrecoverable);
}

// EventServerProxy's RPC-error mapping: kRPCBadBlockID becomes Abandon (the
// block is abandoned, the executor survives), anything else Unrecoverable.
// A swapped mapping would turn an abandonable block into an executor
// fail-stop or vice versa; no prior test asserted either arm on the proxy.
// Also pins the PerTransactionEventServerProxy double-emit guard:
// Unrecoverable with no RPC issued.
TEST(EventServerProxyMapping, BadBlockIdIsAbandonAndDoubleEmitIsUnrecoverable)
{
  NativeContractTestFramework fw(
      false, BlockExecutionMode::kBlockExecutionModeProposal, TestDB::kHogDb);

  EventServerProxy proxy(x23::HostPort{"localhost", fw.get_event_exec_port()});

  const BlockID unknown_block = 987654;

  auto emit_status =
      proxy.emit_event(unknown_block, 0, TxEventLog{}, ProposedTransaction{})();
  ASSERT_FALSE(emit_status.has_value());
  EXPECT_EQ(emit_status.error().code(), SystemErrorCode::Abandon);

  // Double emit on the per-transaction wrapper: the first emit against the
  // real open block succeeds, the second is rejected client-side as
  // Unrecoverable.
  PerTransactionEventServerProxy per_tx(proxy, fw.get_block_number(),
                                        /*tx_id=*/12345);
  auto first = per_tx.emit_events(TxEventLog{}, ProposedTransaction{})();
  ASSERT_TRUE(first.has_value());
  auto second = per_tx.emit_events(TxEventLog{}, ProposedTransaction{})();
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code(), SystemErrorCode::Unrecoverable);
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
