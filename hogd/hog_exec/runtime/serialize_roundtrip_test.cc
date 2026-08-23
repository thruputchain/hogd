#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "built_contracts/contract_loader.h"
#include "built_contracts/cxs_roundtrip.h"
#include "built_contracts/roundtrip.hh"
#include "built_contracts/txs_roundtrip.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/serialize_calldata.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/vec.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTNEXTLINE(misc-multiple-inheritance,misc-use-internal-linkage)
class SerializeRoundtripTest : public ::testing::TestWithParam<TestDB>,
                               public NativeContractTestFramework {
public:
  SerializeRoundtripTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}

protected:
  void SetUp() override
  {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(
        0, [this](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::Foo foo{0xABCD};
          co_await roundtrip_test<u8"roundtrip_foo">(foo, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        1, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::senum senum{::cxs_test::senum::SE_POSITIVE};
          co_await roundtrip_test<u8"roundtrip_senum">(senum, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        2, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::sunion sunion{::cxs_test::senum::SE_POSITIVE};
          sunion.pos() = 5;
          co_await roundtrip_test<u8"roundtrip_sunion">(sunion, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          sunion.d(static_cast<::cxs_test::senum>(0)); // default case
          co_await roundtrip_test<u8"roundtrip_sunion">(sunion, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          sunion.d(static_cast<::cxs_test::senum>(1000)); // default case
          co_await roundtrip_test<u8"roundtrip_sunion">(sunion, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        3, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          int32_t value = -1;
          co_await roundtrip_test<u8"roundtrip_i32">(value, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          value = 1;
          co_await roundtrip_test<u8"roundtrip_i32">(value, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          value = 0;
          co_await roundtrip_test<u8"roundtrip_i32">(value, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          value = std::numeric_limits<int32_t>::max();
          co_await roundtrip_test<u8"roundtrip_i32">(value, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          value = std::numeric_limits<int32_t>::min();
          co_await roundtrip_test<u8"roundtrip_i32">(value, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        4, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::sunion_no_void sunion{::cxs_test::senum::SE_NEGATIVE};
          sunion.neg() = true;
          co_await roundtrip_test<u8"roundtrip_sunion_no_void">(sunion, ctx,
                                                                success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        5, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::uunion uunion(1);
          uunion.one() = true;
          co_await roundtrip_test<u8"roundtrip_uunion">(uunion, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          uunion.d(2);
          uunion.two() = true;
          co_await roundtrip_test<u8"roundtrip_uunion">(uunion, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          uunion.d(4);
          uunion.four() = "hello";
          co_await roundtrip_test<u8"roundtrip_uunion">(uunion, ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        6, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::uunion_bool uunion_bool{true};
          uunion_bool.one() = false;
          co_await roundtrip_test<u8"roundtrip_uunion_bool">(uunion_bool, ctx,
                                                             success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          uunion_bool.d(false);
          uunion_bool.two() = true;
          co_await roundtrip_test<u8"roundtrip_uunion_bool">(uunion_bool, ctx,
                                                             success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }
          co_return InvokeStatus<void>{};
        });
    fns.emplace(
        7, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          ::cxs_test::test_recursive test_recursive;

          // empty
          test_recursive.elem = "hello";
          co_await roundtrip_test<u8"roundtrip_test_recursive">(test_recursive,
                                                                ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // with a box ptr
          test_recursive.next.emplace();
          test_recursive.next->elem = "world";
          co_await roundtrip_test<u8"roundtrip_test_recursive">(test_recursive,
                                                                ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          // with some elements in a vec
          test_recursive.nextvec.emplace_back();
          test_recursive.nextvec.emplace_back();
          test_recursive.nextvec.back().elem = "goodbye";
          co_await roundtrip_test<u8"roundtrip_test_recursive">(test_recursive,
                                                                ctx, success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    // Methods 8, 9, 10 removed: Error, ContractError, StorageError types
    // were removed/restructured in SDK Exceptions PR (#101).
    fns.emplace(
        11, [&](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
          bool success = false;
          x23::Vec<uint8_t> vec = {1, 2, 3, 4, 5};
          co_await roundtrip_test<u8"roundtrip_opaque_vector">(vec, ctx,
                                                               success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_await roundtrip_test<u8"roundtrip_opaque_slice">(vec, ctx,
                                                              success);
          if (!success) {
            co_return InvokeError{InvokeErrorCode::Deterministic};
          }

          co_return InvokeStatus<void>{};
        });
    std::unique_ptr<NativeContract> native_contract =
        std::make_unique<NativeContract>(std::move(fns));

    auto cxs_contract = hogd::load_rv32_contract("cxs_roundtrip.rv32a");
    write_contract(cxs_contract);
    write_deployment(cxs_contract_addr_, cxs_contract);
    setup_grant_storage_token(cxs_contract_addr_, 1'000'000'000);

    auto txs_contract = hogd::load_rv32_contract("txs_roundtrip.rv32a");
    write_contract(txs_contract);
    write_deployment(txs_contract_addr_, txs_contract);
    setup_grant_storage_token(txs_contract_addr_, 1'000'000'000);

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(0, std::move(native_contract));
    auto native_runtime_context =
        std::make_unique<NativeContext>(std::move(native_contract_map));

    // deploy the native contract
    auto native_contract_xdr = make_native_contract(0);
    write_contract(native_contract_xdr);
    write_deployment(native_contract_addr_, native_contract_xdr);
    // The native contract is the root invoked address; each test commits one
    // transaction against it, consuming a replay-cache entry charged here.
    setup_grant_storage_token(native_contract_addr_,
                              replay_cache_cost_charged(1));

    auto rv32_context = std::make_unique<Rv32Context>();
    multiplexed_context_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kNative, {native_runtime_context.release(), true}},
            {ContractType::kRv32, {rv32_context.release(), true}}});
  }

  using cxs_client_t = ContractTraitClient<cxs_roundtrip::CxsRoundtrip>;
  using txs_client_t = ContractTraitClient<txs_roundtrip::TxsRoundtrip>;

  template<typename S>
  S check_result(HostFnStatus<SyscallStatus<std::optional<S>>> res)
  {
    if (!res.has_value() || !res.value().has_value() ||
        !res.value().value().has_value()) {
      panic("Failed to invoke method");
    }
    return res.value().value().value();
  };

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  // Coroutine, so gtest's ASSERT_* (which expand to a bare `return`) cannot be
  // used here; failures report via EXPECT_* and bail out leaving `success`
  // false, which is what the caller checks.
  template<StringLiteral MethodName, typename T>
  x23::Async<void> roundtrip_test(T const &value, NativeSyscallContext &ctx,
                                  bool &success)
  {
    success = false;
    {
      // CXS
      auto calldata = Serializer<SerializationFormat::kCxs>::serialize(value);
      if (!calldata.has_value()) {
        ADD_FAILURE() << "serialize failed";
        co_return;
      }
      auto res = co_await cxs_client_.invoke_opt<MethodName>(ctx, *calldata);
      auto return_buf = check_result(res);
      auto roundtrip_value =
          Serializer<SerializationFormat::kCxs>::deserialize<T>(
              std::as_bytes(std::span(return_buf)));
      if (!roundtrip_value.has_value()) {
        ADD_FAILURE() << "deserialize failed";
        co_return;
      }
      EXPECT_EQ(roundtrip_value.value(), value);
    }
    {
      // TXS
      auto calldata = Serializer<SerializationFormat::kTxs>::serialize(value);
      if (!calldata.has_value()) {
        ADD_FAILURE() << "serialize failed";
        co_return;
      }
      auto res = co_await txs_client_.invoke_opt<MethodName>(ctx, *calldata);
      auto return_buf = check_result(res);
      auto roundtrip_value =
          Serializer<SerializationFormat::kTxs>::deserialize<T>(
              std::as_bytes(std::span(return_buf)));
      if (!roundtrip_value.has_value()) {
        ADD_FAILURE() << "deserialize failed";
        co_return;
      }
      EXPECT_EQ(roundtrip_value.value(), value);
    }
    success = true;
  }
  // NOLINTEND(bugprone-unchecked-optional-access)

  const ContractAddress cxs_contract_addr_ = make_contract_address(0);
  const ContractAddress txs_contract_addr_ = make_contract_address(1);
  const ContractAddress native_contract_addr_ = make_contract_address(2);
  cxs_client_t cxs_client_ = cxs_client_t(cxs_contract_addr_);
  txs_client_t txs_client_ = txs_client_t(txs_contract_addr_);
  std::unique_ptr<MultiplexedContext> multiplexed_context_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_P(SerializeRoundtripTest, SerializeRoundtripFoo)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(0),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripSenum)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(1),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripSunion)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(2),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripI32)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(3),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripSunionNoVoid)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(4),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripUunion)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(5),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripUunionBool)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(6),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

TEST_P(SerializeRoundtripTest, SerializeRoundtripTestRecursive)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(7),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Tests for Error, ContractError, and StorageError roundtrips were removed.
// These types were deleted in the SDK Exceptions PR.

TEST_P(SerializeRoundtripTest, SerializeRoundtripOpaqueVector)
{
  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{
                                    .invoked_address = native_contract_addr_,
                                    .entrypoint = entrypoint_from_method_id(11),
                                    .gas_limit = UINT64_MAX,
                                    .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *multiplexed_context_);
  auto res = handler.root_invoke()();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

namespace {
template<typename Start, typename Intermediate>
void
check_rt(Start value)
{
  auto bytes = Serializer<SerializationFormat::kCxs>::serialize(value);
  ASSERT_TRUE(bytes.has_value());
  auto roundtrip_deserialize =
      Serializer<SerializationFormat::kCxs>::deserialize<Intermediate>(
          std::as_bytes(std::span(*bytes)));
  ASSERT_TRUE(roundtrip_deserialize.has_value());
  int64_t convert = roundtrip_deserialize.value();
  ASSERT_EQ(convert, value);
}
} // namespace

// We want to ensure that a boolean can be upgraded to a int32_t enum.
TEST(FormatConversion, CxsSignToUnsignedRoundtrip)
{
  check_rt<int64_t, uint64_t>(0);
  check_rt<int64_t, uint64_t>(1);
  check_rt<int32_t, bool>(0);
  check_rt<int32_t, bool>(1);

  // Apparently the byte formats of uint32 and int32 are not the same, however.
}

// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(TestDBs, SerializeRoundtripTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDb),
                         test_db_to_string);
// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
