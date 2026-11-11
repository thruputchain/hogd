#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

#ifdef HOGD_EXPERIMENTAL_RISTRETTO_MSM
#include <vector>

#include "ffi_enums.hh"
#endif

namespace hogd {
namespace {

constexpr GasAmount kPlentyOfGas = 100'000'000;

class Ristretto255MsmEntryTest : public ::testing::Test {
  NativeContractTestFramework framework_{
      false, BlockExecutionMode::kBlockExecutionModeProposal, true};

public:
  SyscallResult call(std::span<std::byte> memory, uint32_t input,
                     uint32_t length, uint32_t output,
                     GasAmount gas = kPlentyOfGas)
  {
    auto context = make_hog_exec_context();
    auto handler =
        framework_.make_new_system_call_handler(Transaction{}, *context);
    handler.enable_tracing();
    handler.enable_transaction_stats();
    auto frame = handler.start_frame(
        Invocation{.invoked_address =
                       NativeContractTestFramework::make_contract_address(0),
                   .entrypoint = {},
                   .gas_limit = kPlentyOfGas,
                   .calldata = {}},
        std::to_underlying(InvokeFlag::None), ValidatedAuthTokens{});
    EXPECT_TRUE(frame.has_value());
    auto result =
        syscall_entry(&handler,
                      ExecutionContext{.memory = memory, .gas_remaining = gas},
                      static_cast<uint32_t>(SyscallNo::kRistretto255Msm), input,
                      length, output, 0, 0, 0, 0)();
    const auto *stats = handler.get_transaction_stats();
#ifdef HOGD_EXPERIMENTAL_RISTRETTO_MSM
    EXPECT_EQ(stats->invalid_syscall_count, 0);
    EXPECT_EQ(
        stats->syscall_counts[static_cast<size_t>(SyscallNo::kRistretto255Msm)],
        1);
#else
    EXPECT_EQ(stats->invalid_syscall_count, 1);
    EXPECT_EQ(
        stats->syscall_counts[static_cast<size_t>(SyscallNo::kRistretto255Msm)],
        0);
#endif
    const auto *trace =
        handler.get_syscall_trace_data<SyscallNo::kRistretto255Msm>();
    EXPECT_NE(trace, nullptr);
    if (trace != nullptr) {
      if (result.first.has_value() &&
          *result.first == SyscallErrorCode::kSyscallErrorCodeNone) {
        EXPECT_TRUE(trace->outputs);
        if (trace->outputs) {
          EXPECT_TRUE(std::ranges::equal(std::as_bytes(
                                             std::span(trace->outputs->point)),
                                         memory.subspan(output, 32)));
        }
      }
      else {
        EXPECT_FALSE(trace->outputs);
      }
    }
    EXPECT_TRUE(
        handler.finish_frame(InvokeStatus<void>{}, nullptr)().has_value());
    return result;
  }
};

#ifndef HOGD_EXPERIMENTAL_RISTRETTO_MSM
TEST_F(Ristretto255MsmEntryTest, DisabledNumberSkipsPointerParsing)
{
  std::array<std::byte, 1> memory{std::byte{0xA5}};
  const auto before = memory;
  const auto [result, charged] =
      call(memory, UINT32_MAX, UINT32_MAX, UINT32_MAX);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeNoSyscall);
  EXPECT_EQ(charged, kSyscallBaseCost + kNoSyscallCost);
  EXPECT_EQ(memory, before);
}
#else

constexpr uint32_t kOutput = 0;
constexpr uint32_t kInput = 32;
constexpr std::array<uint8_t, 32> kBasepoint = {
    0xe2, 0xf2, 0xae, 0x0a, 0x6a, 0xbc, 0x4e, 0x71, 0xa8, 0x84, 0xa9,
    0x61, 0xc5, 0x00, 0x51, 0x5f, 0x58, 0xe3, 0x0b, 0x6a, 0xa5, 0x82,
    0xdd, 0x8d, 0xb6, 0xa6, 0x59, 0x45, 0xe0, 0x8d, 0x2d, 0x76,
};
constexpr std::array<uint8_t, 32> kTwiceBasepoint = {
    0x6a, 0x49, 0x32, 0x10, 0xf7, 0x49, 0x9c, 0xd1, 0x7f, 0xec, 0xb5,
    0x10, 0xae, 0x0c, 0xea, 0x23, 0xa1, 0x10, 0xe8, 0xd5, 0xb9, 0x01,
    0xf8, 0xac, 0xad, 0xd3, 0x09, 0x5c, 0x73, 0xa3, 0xb9, 0x19,
};

std::vector<std::byte>
memory_for_terms(size_t terms)
{
  std::vector<std::byte> memory(kInput + terms * kRistretto255MsmTermBytes);
  std::fill_n(memory.begin(), 32, std::byte{0xA5});
  return memory;
}

TEST_F(Ristretto255MsmEntryTest, ProducesCanonicalRfcPoint)
{
  auto memory = memory_for_terms(1);
  memory[kInput] = std::byte{2};
  std::ranges::copy(std::as_bytes(std::span(kBasepoint)),
                    memory.begin() + kInput + 32);
  const auto [result, charged] = call(memory, kInput, 64, kOutput);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeNone);
  EXPECT_EQ(charged, kSyscallBaseCost + GasCost::ristretto255_msm(1));
  EXPECT_TRUE(std::ranges::equal(std::span(memory).first(32),
                                 std::as_bytes(std::span(kTwiceBasepoint))));
}

TEST_F(Ristretto255MsmEntryTest, EmptyAndMaximumIdentitySums)
{
  for (const uint32_t n : {0U, 256U}) {
    auto memory = memory_for_terms(n);
    const auto [result, charged] = call(memory, kInput, n * 64, kOutput);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeNone);
    EXPECT_EQ(charged, kSyscallBaseCost + GasCost::ristretto255_msm(n));
    EXPECT_TRUE(
        std::ranges::all_of(std::span(memory).first(32),
                            [](std::byte b) { return b == std::byte{0}; }));
  }
}

TEST_F(Ristretto255MsmEntryTest, RejectsLengthsWithoutWritingOutput)
{
  for (const uint32_t length : {63U, 257U * 64}) {
    auto memory = memory_for_terms(257);
    const auto before = memory;
    const auto [result, charged] = call(memory, kInput, length, kOutput);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
    EXPECT_EQ(charged, kSyscallBaseCost);
    EXPECT_EQ(memory, before);
  }
}

TEST_F(Ristretto255MsmEntryTest, InvalidScalarAndLatePointPayFullCost)
{
  for (bool bad_scalar : {false, true}) {
    auto memory = memory_for_terms(2);
    if (bad_scalar) {
      std::fill_n(memory.begin() + kInput + 64, 32, std::byte{0xFF});
    }
    else {
      // The malformed second point must be decoded even with scalar zero.
      std::fill_n(memory.begin() + kInput + 64 + 32, 32, std::byte{0xFF});
    }
    const auto before = memory;
    const auto [result, charged] = call(memory, kInput, 128, kOutput);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);
    EXPECT_EQ(charged, kSyscallBaseCost + GasCost::ristretto255_msm(2));
    EXPECT_EQ(memory, before);
  }
}

TEST_F(Ristretto255MsmEntryTest, ReservesFullCostBeforeDecodingOrOutput)
{
  const GasAmount cost = kSyscallBaseCost + GasCost::ristretto255_msm(1);
  for (bool malformed : {false, true}) {
    for (const GasAmount budget :
         {GasAmount{0}, kSyscallBaseCost, cost - 1, cost, cost + 1}) {
      auto memory = memory_for_terms(1);
      if (malformed) {
        std::fill_n(memory.begin() + kInput + 32, 32, std::byte{0xFF});
      }
      const auto before = memory;
      const auto [result, charged] = call(memory, kInput, 64, kOutput, budget);
      if (budget <= cost) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), HostFnErrorCode::OutOfGas);
        EXPECT_EQ(charged, budget);
        EXPECT_EQ(memory, before);
      }
      else {
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result,
                  malformed
                      ? SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput
                      : SyscallErrorCode::kSyscallErrorCodeNone);
        EXPECT_EQ(charged, cost);
        if (malformed) {
          EXPECT_EQ(memory, before);
        }
      }
    }
  }
}

TEST_F(Ristretto255MsmEntryTest, RejectsBadPointersAndOutputAliasing)
{
  for (const auto args : {std::array<uint32_t, 3>{UINT32_MAX, 64, kOutput},
                          std::array<uint32_t, 3>{kInput, 64, UINT32_MAX},
                          std::array<uint32_t, 3>{kInput, 64, kInput}}) {
    auto memory = memory_for_terms(1);
    const auto before = memory;
    const auto [result, charged] = call(memory, args[0], args[1], args[2]);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, SyscallErrorCode::kSyscallErrorCodeFault);
    EXPECT_EQ(charged, kSyscallBaseCost + kGenericBadSyscallCost);
    EXPECT_EQ(memory, before);
  }
}
#endif

} // namespace
} // namespace hogd
