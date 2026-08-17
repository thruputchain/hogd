#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "built_contracts/tuple_return.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/cpputil.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
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
#include "x23/vec.h"
#include "x23/x23.h"

//
// RV32 integration tests for tuple return types.
//
// These tests use a native contract that invokes the tuple_return.rv32 contract
// and decodes the return values to verify that tuple return types are properly
// handled through the entire stack: Rust SDK -> XDR serialization -> C++ header
// generation.
//

namespace hogd {

class TupleReturnRv32Test : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  TupleReturnRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

  template<StringLiteral MethodName, typename Expect, typename... Args>
  static InvokeStatus<void> check_result(NativeSyscallContext &ctx,
                                         Expect const &expected, Args... args)
  {
    using tuple_return_client_t =
        ContractTraitClient<tuple_return::TupleReturn>;
    tuple_return_client_t rv32_client(kRv32ContractAddress);
    auto decoded = rv32_client.template invoke_opt<MethodName>(ctx, args...);
    if (!decoded.has_value() || !decoded.value().has_value() ||
        !decoded.value().value().has_value()) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    auto result = decoded.value().value().value();
    if (result != expected) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  }

protected:
  void SetUp() override
  {
    using tuple_return_meta = ContractTraitMeta<tuple_return::TupleReturn>;

    auto rv32_contract = tuple_return_meta::load_rv32();
    write_contract(rv32_contract);
    write_deployment(kRv32ContractAddress, rv32_contract);
    setup_grant_storage_token(kRv32ContractAddress, 1'000'000'000);

    NativeContractBuilder builder;

    // Method 0: BasicTwoTuple
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        0, [](NativeSyscallContext &ctx, uint32_t input) -> InvokeStatus<void> {
          return check_result<u8"call1">(ctx, std::make_tuple(1U, 2U), input);
        });

    // Method 1: NestedTuple
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        1, [](NativeSyscallContext &ctx, uint32_t input) -> InvokeStatus<void> {
          return check_result<u8"call2">(
              ctx, std::make_tuple(3U, std::make_tuple(4ULL, 5ULL)), input);
        });

    // Method 2: SingleElementTuple
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        2, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"single_element_tuple">(ctx, 42U);
        });

    // Method 3: ThreeElementTuple
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        3, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"three_element_tuple">(
              ctx, std::make_tuple(10U, 20ULL, true));
        });

    // Method 4: DeeplyNested
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        4, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"deeply_nested">(
              ctx,
              std::make_tuple(1U, std::make_tuple(2ULL,
                                                  std::make_tuple(true, -3))));
        });

    // Method 5: TupleWithFixedArray
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        5, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<uint32_t, 4> expected_array = {1, 2, 3, 4};
          return check_result<u8"tuple_with_fixed_array">(
              ctx, std::make_tuple(expected_array, 100ULL));
        });

    // Method 6: TupleWithVariableArray
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        6, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          x23::Vec<uint32_t, 100> expected_vec = {10U, 20U, 30U};
          return check_result<u8"tuple_with_variable_array">(
              ctx, std::make_tuple(expected_vec, 200ULL));
        });

    // Method 7: NestedArrays
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        7, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<uint32_t, 2> expected_arr1 = {5, 6};
          std::array<uint64_t, 3> expected_arr2 = {7, 8, 9};
          return check_result<u8"nested_arrays">(
              ctx, std::make_tuple(expected_arr1, expected_arr2));
        });

    // Method 8: ArrayOfTuples
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        8, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::tuple<uint32_t, uint64_t>, 4> expected = {
              std::make_tuple(1U, 10ULL), std::make_tuple(2U, 20ULL),
              std::make_tuple(3U, 30ULL), std::make_tuple(4U, 40ULL)};
          return check_result<u8"array_of_tuples">(ctx, expected);
        });

    // Method 9: NestedArray2D
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        9, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::array<uint8_t, 4>, 5> expected = {
              std::array<uint8_t, 4>({1, 2, 3, 4}),
              std::array<uint8_t, 4>({5, 6, 7, 8}),
              std::array<uint8_t, 4>({9, 10, 11, 12}),
              std::array<uint8_t, 4>({13, 14, 15, 16}),
              std::array<uint8_t, 4>({17, 18, 19, 20})};
          return check_result<u8"nested_array_2d">(ctx, expected);
        });

    // Method 10: NestedArray3D
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        10, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::array<std::array<uint32_t, 2>, 3>, 4> expected = {};
          expected[0][0] = std::array<uint32_t, 2>({1, 2});
          expected[0][1] = std::array<uint32_t, 2>({3, 4});
          expected[0][2] = std::array<uint32_t, 2>({5, 6});
          expected[1][0] = std::array<uint32_t, 2>({7, 8});
          expected[1][1] = std::array<uint32_t, 2>({9, 10});
          expected[1][2] = std::array<uint32_t, 2>({11, 12});
          expected[2][0] = std::array<uint32_t, 2>({13, 14});
          expected[2][1] = std::array<uint32_t, 2>({15, 16});
          expected[2][2] = std::array<uint32_t, 2>({17, 18});
          expected[3][0] = std::array<uint32_t, 2>({19, 20});
          expected[3][1] = std::array<uint32_t, 2>({21, 22});
          expected[3][2] = std::array<uint32_t, 2>({23, 24});
          return check_result<u8"nested_array_3d">(ctx, expected);
        });

    // Method 11: DeeplyNestedArray
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        11, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<
              std::array<std::array<std::array<std::array<uint8_t, 2>, 2>, 2>,
                         2>,
              2>
              expected = {};
          expected[0][0][0][0] = std::array<uint8_t, 2>({1, 2});
          expected[0][0][0][1] = std::array<uint8_t, 2>({3, 4});
          expected[0][0][1][0] = std::array<uint8_t, 2>({5, 6});
          expected[0][0][1][1] = std::array<uint8_t, 2>({7, 8});
          expected[0][1][0][0] = std::array<uint8_t, 2>({9, 10});
          expected[0][1][0][1] = std::array<uint8_t, 2>({11, 12});
          expected[0][1][1][0] = std::array<uint8_t, 2>({13, 14});
          expected[0][1][1][1] = std::array<uint8_t, 2>({15, 16});
          expected[1][0][0][0] = std::array<uint8_t, 2>({17, 18});
          expected[1][0][0][1] = std::array<uint8_t, 2>({19, 20});
          expected[1][0][1][0] = std::array<uint8_t, 2>({21, 22});
          expected[1][0][1][1] = std::array<uint8_t, 2>({23, 24});
          expected[1][1][0][0] = std::array<uint8_t, 2>({25, 26});
          expected[1][1][0][1] = std::array<uint8_t, 2>({27, 28});
          expected[1][1][1][0] = std::array<uint8_t, 2>({29, 30});
          expected[1][1][1][1] = std::array<uint8_t, 2>({31, 32});
          return check_result<u8"deeply_nested_array">(ctx, expected);
        });

    // Method 12: TupleWithOption
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        12, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"tuple_with_option">(
              ctx, std::make_tuple(x23::Ptr<uint32_t>(123), 456ULL));
        });

    // Method 13: TupleAllOptional
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        13, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"tuple_all_optional">(
              ctx, std::make_tuple(x23::Ptr<uint32_t>(111),
                                   x23::Ptr<uint64_t>(222)));
        });

    // Method 14: NestedOptionTuple
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        14, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          auto expected =
              std::make_tuple(99U, x23::Ptr<std::tuple<uint64_t, bool>>(
                                       std::make_tuple(88ULL, false)));
          return check_result<u8"nested_option_tuple">(ctx, expected);
        });

    // Method 15: KitchenSink
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        15, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<uint64_t, 2> b_array = {10ULL, 20ULL};
          x23::Vec<int, 50> c_vec = {1, 2, 3};
          auto expected =
              std::make_tuple(1U, x23::Ptr<std::array<uint64_t, 2>>(b_array),
                              std::make_tuple(true, c_vec));
          return check_result<u8"kitchen_sink">(ctx, expected);
        });

    // Method 16: ArraysAndTuples
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        16, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::array<std::tuple<uint32_t, uint64_t>, 2>, 3> nested =
              {{{std::make_tuple(1U, 2ULL), std::make_tuple(3U, 4ULL)},
                {std::make_tuple(5U, 6ULL), std::make_tuple(7U, 8ULL)},
                {std::make_tuple(9U, 10ULL), std::make_tuple(11U, 12ULL)}}};
          return check_result<u8"arrays_and_tuples">(ctx,
                                                     std::make_tuple(nested,
                                                                     true));
        });

    // Method 17: TupleWithNestedArray
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        17, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::array<uint32_t, 2>, 3> nested_arr = {
              std::array<uint32_t, 2>({1, 2}), std::array<uint32_t, 2>({3, 4}),
              std::array<uint32_t, 2>({5, 6})};
          return check_result<u8"tuple_with_nested_array">(
              ctx, std::make_tuple(nested_arr, 999ULL));
        });

    // Method 18: Tuple8Elements
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        18, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"tuple_8_elements">(
              ctx, std::make_tuple(1U, 2U, 3U, 4U, 5ULL, 6ULL, 7ULL, 8ULL));
        });

    // Method 19: Tuple16Elements
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        19, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"tuple_16_elements">(
              ctx, std::make_tuple(1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9ULL, 10ULL,
                                   11ULL, 12ULL, 13ULL, 14ULL, 15ULL, 16ULL));
        });

    // Method 20: EmptyTuple
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        20, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          return check_result<u8"empty_tuple">(ctx, std::tuple<>{});
        });

    // Method 21: TupleWithBytes
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        21, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<uint8_t, 32> bytes = {};
          bytes.fill(0xFF);
          return check_result<u8"tuple_with_bytes">(ctx, std::make_tuple(bytes,
                                                                         999U));
        });

    // Method 22: TupleWithVariableBytes
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        22, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          x23::Vec<uint8_t, 1000> bytes = {0xAA, 0xBB, 0xCC, 0xDD};
          return check_result<u8"tuple_with_variable_bytes">(
              ctx, std::make_tuple(bytes, 777U));
        });

    // Method 23: NestedBytesArray
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        23, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::array<uint8_t, 32>, 3> expected = {};
          expected[0].fill(0x11);
          expected[1].fill(0x22);
          expected[2].fill(0x33);
          return check_result<u8"nested_bytes_array">(ctx, expected);
        });

    // Method 24: TupleArrayOption
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        24, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<x23::Ptr<uint64_t>, 2> arr_opt = {x23::Ptr<uint64_t>(10),
                                                       x23::Ptr<uint64_t>(20)};
          return check_result<u8"tuple_array_option">(
              ctx, std::make_tuple(x23::Ptr<std::array<uint32_t, 4>>(
                                       std::array<uint32_t, 4>({1, 2, 3, 4})),
                                   arr_opt));
        });

    // Method 25: NestedEverything
    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        25, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
          std::array<std::array<std::tuple<uint32_t, x23::Ptr<uint64_t>>, 2>, 3>
              nested_all = {};
          nested_all[0][0] = std::make_tuple(1U, x23::Ptr<uint64_t>(10));
          nested_all[0][1] = std::make_tuple(2U, x23::Ptr<uint64_t>(20));
          nested_all[1][0] = std::make_tuple(3U, x23::Ptr<uint64_t>(30));
          nested_all[1][1] = std::make_tuple(4U, x23::Ptr<uint64_t>(40));
          nested_all[2][0] = std::make_tuple(5U, x23::Ptr<uint64_t>(50));
          nested_all[2][1] = std::make_tuple(6U, x23::Ptr<uint64_t>(60));
          auto opt_tuple = x23::Ptr<std::tuple<bool, std::array<int32_t, 4>>>(
              std::make_tuple(true, std::array<int32_t, 4>({-1, -2, -3, -4})));
          return check_result<u8"nested_everything">(
              ctx, std::make_tuple(nested_all, opt_tuple));
        });

    auto native_contract = builder.build();

    std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(0, std::move(native_contract));
    auto native_runtime_context =
        std::make_unique<NativeContext>(std::move(native_contract_map));

    // deploy the native contract
    auto native_contract_xdr = make_native_contract(0);
    write_contract(native_contract_xdr);
    write_deployment(kNativeContractAddress, native_contract_xdr);
    // Each test commits exactly one transaction against this root invoked
    // address (N = 1), and that commit now charges the replay-cache cost.
    setup_grant_storage_token(kNativeContractAddress,
                              replay_cache_cost_charged(1));

    auto rv32_context = std::make_unique<Rv32Context>();
    multiplexed_context_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kNative, {native_runtime_context.release(), true}},
            {ContractType::kRv32, {rv32_context.release(), true}}});
  }

  constexpr static ContractAddress kRv32ContractAddress =
      make_contract_address(0);
  constexpr static ContractAddress kNativeContractAddress =
      make_contract_address(1);
  std::unique_ptr<MultiplexedContext> multiplexed_context_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

//
// Test existing basic two-element tuple.
//
TEST_F(TupleReturnRv32Test, BasicTwoTuple)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              Invocation{
                  .invoked_address = kNativeContractAddress,
                  .entrypoint = entrypoint_from_method_id(0),
                  .gas_limit = UINT64_MAX,
                  .calldata =
                      Serializer<
                          NativeContractBuilder::kDefaultSerializationFormat>::
                          serialize(std::make_tuple(0U))
                              .value()},
          .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test nested tuple.
//
TEST_F(TupleReturnRv32Test, NestedTuple)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              Invocation{
                  .invoked_address = kNativeContractAddress,
                  .entrypoint = entrypoint_from_method_id(1),
                  .gas_limit = UINT64_MAX,
                  .calldata =
                      Serializer<
                          NativeContractBuilder::kDefaultSerializationFormat>::
                          serialize(std::make_tuple(0U))
                              .value()},
          .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test single-element tuple.
//
TEST_F(TupleReturnRv32Test, SingleElementTuple)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(2),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test three-element tuple.
//
TEST_F(TupleReturnRv32Test, ThreeElementTuple)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(3),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test deeply nested tuple.
//
TEST_F(TupleReturnRv32Test, DeeplyNested)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(4),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with fixed array.
//
TEST_F(TupleReturnRv32Test, TupleWithFixedArray)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(5),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with variable array.
//
TEST_F(TupleReturnRv32Test, TupleWithVariableArray)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(6),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test nested arrays in tuple.
//
TEST_F(TupleReturnRv32Test, NestedArrays)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(7),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test array of tuples.
//
TEST_F(TupleReturnRv32Test, ArrayOfTuples)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(8),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test 2D nested array.
//
TEST_F(TupleReturnRv32Test, NestedArray2D)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(9),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test 3D nested array.
//
TEST_F(TupleReturnRv32Test, NestedArray3D)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(10),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test deeply nested 4D array.
//
TEST_F(TupleReturnRv32Test, DeeplyNestedArray)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(11),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with optional.
//
TEST_F(TupleReturnRv32Test, TupleWithOption)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(12),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with all optional elements.
//
TEST_F(TupleReturnRv32Test, TupleAllOptional)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(13),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test nested optional tuple.
//
TEST_F(TupleReturnRv32Test, NestedOptionTuple)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(14),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test kitchen sink with complex combinations.
//
TEST_F(TupleReturnRv32Test, KitchenSink)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(15),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test arrays and tuples combination.
//
TEST_F(TupleReturnRv32Test, ArraysAndTuples)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(16),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with nested array.
//
TEST_F(TupleReturnRv32Test, TupleWithNestedArray)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(17),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test 8-element tuple.
//
TEST_F(TupleReturnRv32Test, Tuple8Elements)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(18),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test 16-element tuple (maximum supported).
//
TEST_F(TupleReturnRv32Test, Tuple16Elements)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(19),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test empty tuple (void).
//
TEST_F(TupleReturnRv32Test, EmptyTuple)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(20),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with fixed byte array.
//
TEST_F(TupleReturnRv32Test, TupleWithBytes)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(21),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with variable byte array.
//
TEST_F(TupleReturnRv32Test, TupleWithVariableBytes)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(22),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test nested bytes array.
//
TEST_F(TupleReturnRv32Test, NestedBytesArray)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(23),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test tuple with array of optionals.
//
TEST_F(TupleReturnRv32Test, TupleArrayOption)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(24),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

//
// Test nested everything - maximum complexity.
//
TEST_F(TupleReturnRv32Test, NestedEverything)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      Invocation{.invoked_address = kNativeContractAddress,
                                 .entrypoint = entrypoint_from_method_id(25),
                                 .gas_limit = UINT64_MAX,
                                 .calldata = {}},
                  .uniquifier = make_new_uniquifier()},
      *multiplexed_context_);
  auto res = handler.root_invoke();
  ASSERT_TRUE(res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
