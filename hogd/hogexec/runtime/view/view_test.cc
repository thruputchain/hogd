#include <cstddef>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "auth_claim.hh"
#include "built_contracts/fibonacci.h"
#include "built_contracts/read_only_test.h"
#include "contract.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/view/view.h"
#include "invoke_flag.hh"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "x23/cleanup.h"
#include "x23/vec.h"

namespace hogd {

class ViewTest : public ::testing::Test {
public:
  using fibonacci_meta = ContractTraitMeta<fibonacci::Fibonacci>;
  using read_only_meta = ContractTraitMeta<read_only_test::ReadOnlyTest>;

  static constexpr ContractAddress kFibAddress =
      NativeContractTestFramework::make_contract_address(1);
  static constexpr ContractAddress kReadOnlyAddress =
      NativeContractTestFramework::make_contract_address(2);

  void SetUp() override
  {
    auto fib_contract = fibonacci_meta::load_wasm();
    framework_.write_contract(fib_contract);
    framework_.write_deployment(kFibAddress, fib_contract);
    framework_.setup_grant_storage_token(kFibAddress, 1'000'000'000);

    auto ro_contract = read_only_meta::load_wasm();
    framework_.write_contract(ro_contract);
    framework_.write_deployment(kReadOnlyAddress, ro_contract);
    framework_.setup_grant_storage_token(kReadOnlyAddress, 1'000'000'000);
  }

protected:
  ExpectedViewResult run_view(TransactionViewQuery const &query)
  {
    static auto context = make_hogexec_context();
    return run_view(query, *context);
  }

  ExpectedViewResult run_view(TransactionViewQuery const &query,
                              RuntimeContext &context,
                              NondeterministicProposalData ndet = {})
  {
    return View::run_view_transaction(
        *framework_.get_storage_snapshot_cache(),
        framework_.make_new_per_tx_db_proxy(
            std::numeric_limits<TransactionID>::max()),
        framework_.make_new_per_tx_event_server_proxy(
            std::numeric_limits<TransactionID>::max()),
        query, context, std::move(ndet));
  }

  static TransactionViewQuery make_query(Invocation invocation,
                                         AuthClaim auth_claim = {})
  {
    return TransactionViewQuery{
        .invocation = std::move(invocation),
        .parent_address = {},
        .auth_claim = std::move(auth_claim),
        .forwarded_tokens = {},
        .invoke_flags = {},
    };
  }

  static TransactionViewQuery
  make_query_with_parent(Invocation invocation, ContractAddress parent_address,
                         AuthClaim auth_claim = {})
  {
    return TransactionViewQuery{
        .invocation = std::move(invocation),
        .parent_address = std::move(parent_address),
        .auth_claim = std::move(auth_claim),
        .forwarded_tokens = {},
        .invoke_flags = {},
    };
  }

  static x23::Vec<uint8_t, kMaxEventLength>
  make_event_vec(std::string_view data)
  {
    return x23::Vec<uint8_t, kMaxEventLength>(
        reinterpret_cast<uint8_t const *>(data.data()),
        reinterpret_cast<uint8_t const *>(data.data() + data.size()));
  }

  static InvokeStatus<void> emit_event(NativeSyscallContext &ctx,
                                       std::string const &payload)
  {
    auto s = ctx.syscall<SyscallNo::kLogEvent>(
        static_cast<SHA256Hash const *>(nullptr),
        static_cast<SHA256Hash const *>(nullptr),
        static_cast<SHA256Hash const *>(nullptr),
        std::as_bytes(std::span(payload)));
    if (!s.has_value()) {
      return InvokeError{InvokeErrorCode::Unrecoverable};
    }
    if (s.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  }

  struct NativeViewSetup {
    std::unique_ptr<NativeContext> native_context;
    std::unique_ptr<MultiplexedContext> context;

    NativeViewSetup(
        std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns)
    {
      auto nc = std::make_unique<NativeContract>(std::move(fns));
      std::unordered_map<int32_t, std::unique_ptr<NativeContract>> map;
      map.emplace(0, std::move(nc));
      native_context = std::make_unique<NativeContext>(std::move(map));
      auto wasm_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
      context = std::make_unique<MultiplexedContext>(
          std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
              {ContractType::kWasmi, {wasm_context.release(), true}},
              {ContractType::kNative, {native_context.get(), false}}});
    }
  };

  void deploy_native(ContractAddress addr)
  {
    auto native_xdr = NativeContractTestFramework::make_native_contract(0);
    framework_.write_contract(native_xdr);
    framework_.write_deployment(addr, native_xdr);
    framework_.setup_grant_storage_token(addr, 1'000'000'000);
  }

  NativeContractTestFramework framework_;

  x23::Defer<> force_syscallhandler_commit_ =
      SystemCallHandler::force_commit_or_abort();
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(ViewTest, BasicViewReturnsOkWithCorrectOutput)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 10U);

  auto result = run_view(make_query(invocation));
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);

  auto &buf = result.result().output.return_buffer();
  auto decoded = fibonacci_meta::decode_return<"fibonacci">(
      std::as_bytes(std::span(buf.data(), buf.size())), false);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, 55U);
}

TEST_F(ViewTest, ViewGasExhaustion)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(1, 10U);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Fault);
}

TEST_F(ViewTest, ViewWithMsgSender)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 5U);

  auto result = run_view(make_query_with_parent(invocation, kReadOnlyAddress));
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewNonexistentContract)
{
  ContractAddress bad_address =
      NativeContractTestFramework::make_contract_address(99);
  ContractTraitClient<fibonacci::Fibonacci> client(bad_address);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 10U);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::InvalidInput);
}

TEST_F(ViewTest, ViewReentrantParentMatchesInvoked)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 5U);

  auto result = run_view(make_query_with_parent(invocation, kFibAddress));
  EXPECT_EQ(result.status(), TransactionViewStatus::InvalidInput);
  EXPECT_EQ(result.error_code(), SyscallErrorCode::kSyscallErrorCodeReentrance);
}

TEST_F(ViewTest, ViewContractSelfInvokeFaults)
{
  ContractTraitClient<read_only_test::ReadOnlyTest> client(kReadOnlyAddress);
  auto invocation =
      client.make_invocation<"read_only_aliasing_before_read_only_check">(
          UINT64_MAX);

  auto result = run_view(make_query(invocation));
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output,
            InvocationFrameOutput(InvocationFrameStatusCode::Fault));
}

TEST_F(ViewTest, ViewNativeContractGetMsgSenderIsRoot)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          ContractAddress sender{};
          auto r = ctx.syscall<SyscallNo::kGetMsgSender>(sender);
          if (!r.has_value()) {
            ADD_FAILURE() << "get_msg_sender syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() !=
              SyscallErrorCode::kSyscallErrorCodeIsRootTransaction) {
            ADD_FAILURE() << "expected IsRootTransaction";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewNativeContractGetMsgSenderWithParent)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kParentAddr =
      NativeContractTestFramework::make_contract_address(20);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          ContractAddress sender{};
          auto r = ctx.syscall<SyscallNo::kGetMsgSender>(sender);
          if (!r.has_value()) {
            ADD_FAILURE() << "get_msg_sender syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "expected successful get_msg_sender";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          EXPECT_EQ(sender, kParentAddr);
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query_with_parent(invocation, kParentAddr), *setup.context);
  EXPECT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewFrameReverted)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto ret = ctx.syscall<SyscallNo::kReturn>(
              std::as_bytes(std::span("hello", 5)), true,
              std::span<const std::byte>{});
          if (ret.has_value() ||
              ret.error().code() != HostFnErrorCode::Return) {
            ADD_FAILURE() << "expected Return error from kReturn syscall";
          }
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Abort);
}

TEST_F(ViewTest, ViewOutputCorrectWithParentFrame)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kParentAddr =
      NativeContractTestFramework::make_contract_address(20);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          uint32_t val = 42;
          auto ret = ctx.syscall<SyscallNo::kReturn>(
              std::as_bytes(
                  std::span(reinterpret_cast<const char *>(&val), sizeof(val))),
              false, std::span<const std::byte>{});
          if (ret.has_value() ||
              ret.error().code() != HostFnErrorCode::Return) {
            ADD_FAILURE() << "expected Return from kReturn syscall";
          }
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query_with_parent(invocation, kParentAddr), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);

  auto &buf = result.result().output.return_buffer();
  EXPECT_FALSE(buf.empty());
}

TEST_F(ViewTest, ViewDeterministicWasm)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return InvokeError{InvokeErrorCode::Deterministic};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Fault);
}

TEST_F(ViewTest, ViewTryInvokeSucceedsWhenDefaulted)
{
  static constexpr ContractAddress kCallerAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kCalleeAddr =
      NativeContractTestFramework::make_contract_address(11);

  NativeViewSetup setup(
      {{0,
        [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return {};
        }},
       {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kCalleeAddr, 0, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            ADD_FAILURE() << "try_invoke syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke returned error: "
                          << static_cast<int>(r.value());
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        }}});

  deploy_native(kCallerAddr);
  deploy_native(kCalleeAddr);

  Invocation invocation{
      .invoked_address = kCallerAddr,
      .entrypoint = entrypoint_from_method_id(1),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);
}

TEST_F(ViewTest, ViewTryInvokeSucceedsWhenSuppliedTrue)
{
  static constexpr ContractAddress kCallerAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kCalleeAddr =
      NativeContractTestFramework::make_contract_address(11);

  NativeViewSetup setup(
      {{0,
        [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return {};
        }},
       {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kCalleeAddr, 0, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            ADD_FAILURE() << "try_invoke syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke returned error: "
                          << static_cast<int>(r.value());
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        }}});

  deploy_native(kCallerAddr);
  deploy_native(kCalleeAddr);

  NondeterministicProposalData ndet{};
  ndet.nondeterministic_bits.push_back(0x01);
  ndet.nondeterministic_bit_count = 1;

  Invocation invocation{
      .invoked_address = kCallerAddr,
      .entrypoint = entrypoint_from_method_id(1),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query(invocation), *setup.context, std::move(ndet));
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);
}

TEST_F(ViewTest, ViewTryInvokeFailsWhenSuppliedFalse)
{
  static constexpr ContractAddress kCallerAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kCalleeAddr =
      NativeContractTestFramework::make_contract_address(11);

  NativeViewSetup setup(
      {{0,
        [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return {};
        }},
       {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kCalleeAddr, 0, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::TryInvoke), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            ADD_FAILURE() << "try_invoke syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() == SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "try_invoke should have failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        }}});

  deploy_native(kCallerAddr);
  deploy_native(kCalleeAddr);

  NondeterministicProposalData ndet{};
  ndet.nondeterministic_bits.push_back(0x00);
  ndet.nondeterministic_bit_count = 1;

  Invocation invocation{
      .invoked_address = kCallerAddr,
      .entrypoint = entrypoint_from_method_id(1),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query(invocation), *setup.context, std::move(ndet));
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);
}

TEST_F(ViewTest, ViewRcqConsumeDefaultNotPresent)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto key = NativeContractTestFramework::make_storage_key("rcq");
          QueueEntry entry;
          RuntimeMemoryLength written_len = 0;
          auto r = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              key, false, std::as_writable_bytes(std::span(entry)),
              written_len);
          if (!r.has_value()) {
            ADD_FAILURE() << "rcq consume syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() !=
              SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
            ADD_FAILURE() << "expected NoRetrocausalQueueEntry, got "
                          << static_cast<int>(r.value());
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewRcqConsumeSuppliedNotPresent)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto key = NativeContractTestFramework::make_storage_key("rcq");
          QueueEntry entry;
          RuntimeMemoryLength written_len = 0;
          auto r = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              key, false, std::as_writable_bytes(std::span(entry)),
              written_len);
          if (!r.has_value()) {
            ADD_FAILURE() << "rcq consume syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() !=
              SyscallErrorCode::kSyscallErrorCodeNoRetrocausalQueueEntry) {
            ADD_FAILURE() << "expected NoRetrocausalQueueEntry, got "
                          << static_cast<int>(r.value());
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        }}});

  deploy_native(kNativeAddr);

  NondeterministicProposalData ndet{};
  ndet.nondeterministic_bits.push_back(0x00);
  ndet.nondeterministic_bit_count = 1;

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query(invocation), *setup.context, std::move(ndet));
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewRcqConsumeSuppliedPresent)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto key = NativeContractTestFramework::make_storage_key("rcq");
          QueueEntry entry = {0};
          RuntimeMemoryLength written_len = 0;
          auto r = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              key, false, std::as_writable_bytes(std::span(entry)),
              written_len);
          if (!r.has_value()) {
            ADD_FAILURE() << "rcq consume syscall failed";
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            ADD_FAILURE() << "expected successful consume, got "
                          << static_cast<int>(r.value());
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          EXPECT_GT(written_len, 0U);
          EXPECT_EQ(entry[0], 0xAB);
          return {};
        }}});

  deploy_native(kNativeAddr);

  NondeterministicProposalData ndet{};
  ndet.nondeterministic_bits.push_back(0x01);
  ndet.nondeterministic_bit_count = 1;
  QueueEntry supplied_entry;
  supplied_entry.push_back(0xAB);
  supplied_entry.push_back(0xCD);
  ndet.observed_queue_entry_sequence.push_back(supplied_entry);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query(invocation), *setup.context, std::move(ndet));
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
}

TEST_F(ViewTest, ViewRcqConsumePresentButNoEntryData)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto key = NativeContractTestFramework::make_storage_key("rcq");
          QueueEntry entry;
          RuntimeMemoryLength written_len = 0;
          auto r = ctx.syscall<SyscallNo::kRetrocausalQueueConsume>(
              key, false, std::as_writable_bytes(std::span(entry)),
              written_len);
          if (r.has_value()) {
            ADD_FAILURE() << "rcq consume should not have succeeded";
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (r.error().code() != HostFnErrorCode::ValidationFailure) {
            ADD_FAILURE() << "expected ValidationFailure, got "
                          << static_cast<int>(r.error().code());
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          return InvokeError{InvokeErrorCode::ValidationFailure};
        }}});

  deploy_native(kNativeAddr);

  NondeterministicProposalData ndet{};
  ndet.nondeterministic_bits.push_back(0x01);
  ndet.nondeterministic_bit_count = 1;

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result =
      run_view(make_query(invocation), *setup.context, std::move(ndet));
  EXPECT_EQ(result.status(),
            TransactionViewStatus::NondeterministicDataValidationFailure);
}

TEST_F(ViewTest, ViewSingleFrameNoEvents)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);
  EXPECT_TRUE(result.result().events.empty());
}

TEST_F(ViewTest, ViewSingleFrameEmitsMultipleEvents)
{
  static constexpr ContractAddress kNativeAddr =
      NativeContractTestFramework::make_contract_address(10);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          for (auto const *payload : {"first", "second", "third"}) {
            auto r = emit_event(ctx, std::string(payload));
            if (!r.has_value()) {
              return r;
            }
          }
          return {};
        }}});

  deploy_native(kNativeAddr);

  Invocation invocation{
      .invoked_address = kNativeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);

  auto const &events = result.result().events;
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].contract_address, kNativeAddr);
  EXPECT_EQ(events[0].num_topics, 0U);
  EXPECT_EQ(events[0].serialized_event, make_event_vec("first"));
  EXPECT_EQ(events[1].contract_address, kNativeAddr);
  EXPECT_EQ(events[1].num_topics, 0U);
  EXPECT_EQ(events[1].serialized_event, make_event_vec("second"));
  EXPECT_EQ(events[2].contract_address, kNativeAddr);
  EXPECT_EQ(events[2].num_topics, 0U);
  EXPECT_EQ(events[2].serialized_event, make_event_vec("third"));
}

TEST_F(ViewTest, ViewNestedFrameEventsCombinedInOrder)
{
  // Caller (method 1) emits "outer_before", invokes callee (method 0) which
  // emits "inner_leaf", then caller emits "outer_after". Events from the
  // nested frame must be interleaved with the caller's events in invocation
  // order and tagged with each emitter's contract address.

  static constexpr ContractAddress kCallerAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kCalleeAddr =
      NativeContractTestFramework::make_contract_address(11);

  NativeViewSetup setup(
      {{0,
        [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return emit_event(ctx, "inner_leaf");
        }},
       {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          if (auto r = emit_event(ctx, "outer_before"); !r.has_value()) {
            return r;
          }
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kCalleeAddr, 0, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return emit_event(ctx, "outer_after");
        }}});

  deploy_native(kCallerAddr);
  deploy_native(kCalleeAddr);

  Invocation invocation{
      .invoked_address = kCallerAddr,
      .entrypoint = entrypoint_from_method_id(1),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);

  auto const &events = result.result().events;
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].contract_address, kCallerAddr);
  EXPECT_EQ(events[0].serialized_event, make_event_vec("outer_before"));
  EXPECT_EQ(events[1].contract_address, kCalleeAddr);
  EXPECT_EQ(events[1].serialized_event, make_event_vec("inner_leaf"));
  EXPECT_EQ(events[2].contract_address, kCallerAddr);
  EXPECT_EQ(events[2].serialized_event, make_event_vec("outer_after"));
}

TEST_F(ViewTest, ViewDeeplyNestedFrameEventsCombinedInOrder)
{
  // Three-level nesting: outer (method 2) -> middle (method 1) -> leaf
  // (method 0). Each frame emits one event before its child invoke and one
  // after. The leaf emits a single event. Resulting order:
  // outer_pre, middle_pre, leaf, middle_post, outer_post.

  static constexpr ContractAddress kOuterAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kMiddleAddr =
      NativeContractTestFramework::make_contract_address(11);
  static constexpr ContractAddress kLeafAddr =
      NativeContractTestFramework::make_contract_address(12);

  NativeViewSetup setup(
      {{0,
        [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return emit_event(ctx, "leaf");
        }},
       {1,
        [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          if (auto r = emit_event(ctx, "middle_pre"); !r.has_value()) {
            return r;
          }
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kLeafAddr, 0, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return emit_event(ctx, "middle_post");
        }},
       {2, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          if (auto r = emit_event(ctx, "outer_pre"); !r.has_value()) {
            return r;
          }
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kMiddleAddr, 1, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return emit_event(ctx, "outer_post");
        }}});

  deploy_native(kOuterAddr);
  deploy_native(kMiddleAddr);
  deploy_native(kLeafAddr);

  Invocation invocation{
      .invoked_address = kOuterAddr,
      .entrypoint = entrypoint_from_method_id(2),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);

  auto const &events = result.result().events;
  ASSERT_EQ(events.size(), 5U);
  EXPECT_EQ(events[0].contract_address, kOuterAddr);
  EXPECT_EQ(events[0].serialized_event, make_event_vec("outer_pre"));
  EXPECT_EQ(events[1].contract_address, kMiddleAddr);
  EXPECT_EQ(events[1].serialized_event, make_event_vec("middle_pre"));
  EXPECT_EQ(events[2].contract_address, kLeafAddr);
  EXPECT_EQ(events[2].serialized_event, make_event_vec("leaf"));
  EXPECT_EQ(events[3].contract_address, kMiddleAddr);
  EXPECT_EQ(events[3].serialized_event, make_event_vec("middle_post"));
  EXPECT_EQ(events[4].contract_address, kOuterAddr);
  EXPECT_EQ(events[4].serialized_event, make_event_vec("outer_post"));
}

TEST_F(ViewTest, ViewRevertedChildFrameEventsDropped)
{
  // Caller (method 1) emits "before", invokes callee (method 0) which emits
  // "child_dropped" then manually reverts. Caller observes FrameReverted and
  // emits "after". The reverted child frame's event must be dropped; only the
  // caller's two events survive in the view's events list.

  static constexpr ContractAddress kCallerAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kCalleeAddr =
      NativeContractTestFramework::make_contract_address(11);

  NativeViewSetup setup(
      {{0,
        [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          if (auto r = emit_event(ctx, "child_dropped"); !r.has_value()) {
            return r;
          }
          auto ret =
              ctx.syscall<SyscallNo::kReturn>(std::span<const std::byte>{},
                                              true,
                                              std::span<const std::byte>{});
          if (ret.has_value() ||
              ret.error().code() != HostFnErrorCode::Return) {
            ADD_FAILURE() << "expected Return error from kReturn syscall";
          }
          return {};
        }},
       {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          if (auto r = emit_event(ctx, "before"); !r.has_value()) {
            return r;
          }
          RuntimeMemoryLength out_buf_len = 0;
          RuntimeMemoryLength out_auth_len = 0;
          auto r = ctx.syscall<SyscallNo::kInvoke>(
              kCalleeAddr, 0, UINT64_MAX, Calldata{},
              std::to_underlying(InvokeFlag::None), std::span<uint32_t>{},
              out_buf_len, out_auth_len);
          if (!r.has_value()) {
            return InvokeError{InvokeErrorCode::Unrecoverable};
          }
          if (r.value() != SyscallErrorCode::kSyscallErrorCodeFrameReverted) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return emit_event(ctx, "after");
        }}});

  deploy_native(kCallerAddr);
  deploy_native(kCalleeAddr);

  Invocation invocation{
      .invoked_address = kCallerAddr,
      .entrypoint = entrypoint_from_method_id(1),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto result = run_view(make_query(invocation), *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);

  auto const &events = result.result().events;
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].contract_address, kCallerAddr);
  EXPECT_EQ(events[0].serialized_event, make_event_vec("before"));
  EXPECT_EQ(events[1].contract_address, kCallerAddr);
  EXPECT_EQ(events[1].serialized_event, make_event_vec("after"));
}

TEST_F(ViewTest, ViewForwardedTokenWithoutParentReturnsInvalidInput)
{
  ContractTraitClient<fibonacci::Fibonacci> client(kFibAddress);
  auto invocation = client.make_invocation<"fibonacci">(UINT64_MAX, 10U);
  auto query = make_query(invocation);
  query.forwarded_tokens.push_back(
      AuthToken{.mint_address = kFibAddress,
                .expiry_height = 100,
                .root_node = {.consumer_address = kFibAddress,
                              .payload = {},
                              .child_nodes = {}}});

  auto result = run_view(query);
  EXPECT_EQ(result.status(), TransactionViewStatus::InvalidInput);
  EXPECT_EQ(result.error_code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
}

TEST_F(ViewTest, ViewForwardedTokenWithParentSucceeds)
{
  static constexpr ContractAddress kCalleeAddr =
      NativeContractTestFramework::make_contract_address(10);
  static constexpr ContractAddress kParentAddr =
      NativeContractTestFramework::make_contract_address(20);

  NativeViewSetup setup(
      {{0, [](NativeSyscallContext & /*unused*/) -> InvokeStatus<void> {
          return {};
        }}});

  deploy_native(kCalleeAddr);

  Invocation invocation{
      .invoked_address = kCalleeAddr,
      .entrypoint = entrypoint_from_method_id(0),
      .gas_limit = UINT64_MAX,
      .calldata = {},
  };

  auto query = make_query_with_parent(invocation, kParentAddr);
  query.forwarded_tokens.push_back(
      AuthToken{.mint_address = kParentAddr,
                .expiry_height = 1'000'000,
                .root_node = {.consumer_address = kCalleeAddr,
                              .payload = {},
                              .child_nodes = {}}});

  auto result = run_view(query, *setup.context);
  ASSERT_EQ(result.status(), TransactionViewStatus::Ok);
  EXPECT_EQ(result.result().output.status_code(),
            InvocationFrameStatusCode::Return);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
