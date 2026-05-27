#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/syscall_entry.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace {

constexpr uint32_t kFrameAddress = 0;
constexpr uint32_t kInputAddress = 128;
constexpr uint32_t kOutputAddress = 192;
constexpr uint64_t kPlentyOfGas = 1'000'000;

void
store_le64(std::span<uint8_t> bytes, uint32_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

int32_t
load_le32(std::span<const uint8_t> bytes, uint32_t offset)
{
  uint32_t value = 0;
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    value |= uint32_t{bytes[offset + i]} << (i * 8);
  }
  return static_cast<int32_t>(value);
}

uint64_t
load_le64(std::span<const uint8_t> bytes, uint32_t offset)
{
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    value |= uint64_t{bytes[offset + i]} << (i * 8);
  }
  return value;
}

void
write_frame(std::span<uint8_t> memory, std::span<const uint64_t> words)
{
  ASSERT_LE(words.size(), hogd::rv32::kSyscallFrameArgWords);
  for (size_t i = 0; i < words.size(); ++i) {
    store_le64(memory, kFrameAddress + i * sizeof(uint64_t), words[i]);
  }
}

} // namespace

namespace hogd {
namespace {

class Rv32SyscallEntryBridgeTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  Rv32SyscallEntryBridgeTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  void SetUp() override { runtime_context_ = make_hogexec_context(); }

  SystemCallHandler make_handler()
  {
    return make_new_system_call_handler(Transaction{}, *runtime_context_);
  }

  static std::unique_ptr<InvocationFrameTrace>
  start_frame(SystemCallHandler &handler)
  {
    return handler.start_frame(Invocation{
                                   .invoked_address = make_contract_address(0),
                                   .entrypoint = ContractEntrypointID{},
                                   .gas_limit = kPlentyOfGas,
                                   .calldata = Calldata{},
                               },
                               std::to_underlying(InvokeFlag::None),
                               std::span<const uint32_t>{});
  }

  static void finish_frame(
      SystemCallHandler &handler,
      std::unique_ptr<InvocationFrameTrace> stashed_parent_trace,
      InvokeStatus<void> result = {})
  {
    auto status =
        handler.finish_frame(std::move(result), std::move(stashed_parent_trace));
    ASSERT_TRUE(status.has_value());
  }

  static rv32::HostResult call_bridge(std::span<uint8_t> memory,
                                      SystemCallHandler &handler,
                                      uint64_t gas_remaining)
  {
    auto host_context = make_new_host_call_context(&handler);
    rv32::SyscallBridgeHost bridge(host_context.get(), &syscall_entry);
    std::array<uint32_t, rv32::kRegisterCount> registers{};
    rv32::MachineView view(0, gas_remaining, registers, memory);
    return bridge.ecall(view, rv32::EcallArgs{
                                  .a0 = kFrameAddress,
                                  .a7 = rv32::kSyscallBridgeVersion,
                              });
  }

  std::unique_ptr<RuntimeContext> runtime_context_;
};

TEST_F(Rv32SyscallEntryBridgeTest, Sha256UsesRealSyscallEntry)
{
  std::vector<uint8_t> memory(256);
  constexpr std::string_view input = "rv32 syscall bridge";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  constexpr std::array<uint64_t, 4> frame_words = {
      static_cast<uint64_t>(SyscallNo::kSHA256),
      kInputAddress,
      input.size(),
      kOutputAddress,
  };
  write_frame(memory, frame_words);

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result = call_bridge(memory, handler, kPlentyOfGas);
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(result.value,
            static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeNone));
  const SHA256Hash expected = sha256(input);
  EXPECT_TRUE(std::ranges::equal(
      expected, std::span<const uint8_t>(memory).subspan(kOutputAddress,
                                                         expected.size())));
  EXPECT_EQ(load_le32(memory, rv32::kSyscallFrameResultOffset),
            static_cast<int32_t>(SyscallErrorCode::kSyscallErrorCodeNone));
  EXPECT_EQ(load_le32(memory, rv32::kSyscallFrameResultOffset + 4),
            static_cast<int32_t>(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_GT(load_le64(memory, rv32::kSyscallFrameResultOffset + 8), 0U);
}

TEST_F(Rv32SyscallEntryBridgeTest, ParserFaultWritesRecoverableResult)
{
  std::vector<uint8_t> memory(256, 0xAA);
  constexpr std::string_view input = "fault";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  const std::array<uint64_t, 4> frame_words = {
      static_cast<uint64_t>(SyscallNo::kSHA256),
      kInputAddress,
      input.size(),
      memory.size() - 1,
  };
  write_frame(memory, frame_words);

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result = call_bridge(memory, handler, kPlentyOfGas);
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(result.value,
            static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeFault));
  EXPECT_EQ(load_le32(memory, rv32::kSyscallFrameResultOffset),
            static_cast<int32_t>(SyscallErrorCode::kSyscallErrorCodeFault));
  EXPECT_EQ(load_le32(memory, rv32::kSyscallFrameResultOffset + 4),
            static_cast<int32_t>(HostFnErrorCode::NoneOrRecoverable));
}

TEST_F(Rv32SyscallEntryBridgeTest, InvalidSyscallUsesNoSyscallFallback)
{
  std::vector<uint8_t> memory(256);
  constexpr std::array<uint64_t, 1> frame_words = {
      static_cast<uint64_t>(kSyscallCount),
  };
  write_frame(memory, frame_words);

  auto handler = make_handler();
  handler.enable_transaction_stats();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result = call_bridge(memory, handler, kPlentyOfGas);
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(
      result.value,
      static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeNoSyscall));
  EXPECT_EQ(load_le32(memory, rv32::kSyscallFrameResultOffset),
            static_cast<int32_t>(
                SyscallErrorCode::kSyscallErrorCodeNoSyscall));
  EXPECT_EQ(load_le32(memory, rv32::kSyscallFrameResultOffset + 4),
            static_cast<int32_t>(HostFnErrorCode::NoneOrRecoverable));
  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 1);
}

TEST_F(Rv32SyscallEntryBridgeTest, OutOfGasDoesNotWriteBackGuestMemory)
{
  std::vector<uint8_t> memory(256, 0xCC);
  constexpr std::string_view input = "out of gas";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  constexpr std::array<uint64_t, 4> frame_words = {
      static_cast<uint64_t>(SyscallNo::kSHA256),
      kInputAddress,
      input.size(),
      kOutputAddress,
  };
  write_frame(memory, frame_words);
  const std::vector<uint8_t> before = memory;

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result = call_bridge(memory, handler, 0);
  finish_frame(handler, std::move(stashed_trace),
               std::unexpected(InvokeError{InvokeErrorCode::OutOfGas}));

  EXPECT_EQ(result.status, rv32::HostStatus::kOutOfGas);
  EXPECT_EQ(memory, before);
}

} // namespace
} // namespace hogd
