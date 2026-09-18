#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/machine.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/syscall_bridge.h"
#include "hogd/hog_exec/runtime/rv32/threaded_machine.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "invoke_args.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "transaction_trace.hh"
#include "types.hh"

// Integration test fixtures use descriptive local constexpr names; keep the
// code close to the syscall frame vocabulary.
// NOLINTBEGIN(readability-identifier-naming)

namespace {

constexpr uint32_t kInputAddress = 128;
constexpr uint32_t kOutputAddress = 192;
constexpr uint64_t kPlentyOfGas = 1'000'000;
constexpr size_t kRv32SyscallArgumentWords = 7;

static_assert(std::is_standard_layout_v<hogd::rv32::EcallArgs>);
static_assert(sizeof(hogd::rv32::EcallArgs) ==
              (1 + kRv32SyscallArgumentWords) * sizeof(uint32_t));
static_assert(offsetof(hogd::rv32::EcallArgs, a0) == 0 * sizeof(uint32_t));
static_assert(offsetof(hogd::rv32::EcallArgs, a1) == 1 * sizeof(uint32_t));
static_assert(offsetof(hogd::rv32::EcallArgs, a7) == 7 * sizeof(uint32_t));

// Register-native ABI: the syscall number and up to seven argument words are
// placed in a0..a7. Build the EcallArgs the bridge consumes from raw register
// contents.
hogd::rv32::EcallArgs
make_ecall_args(std::span<const uint32_t> words)
{
  hogd::rv32::EcallArgs args = {};
  uint32_t *slots[] = {&args.a0, &args.a1, &args.a2, &args.a3,
                       &args.a4, &args.a5, &args.a6, &args.a7};
  constexpr size_t kMaxArgs = 1 + kRv32SyscallArgumentWords;
  EXPECT_LE(words.size(), kMaxArgs);
  for (size_t i = 0; i < words.size() && i < kMaxArgs; ++i) {
    *slots[i] = words[i];
  }
  return args;
}

} // namespace

namespace hogd {
namespace {

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

uint32_t
i_type(uint32_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return (static_cast<uint32_t>(imm) & 0xfffU) << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | opcode;
}

uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x13, rd, 0, rs1, imm);
}

uint32_t
ecall()
{
  return 0x00000073U;
}

// Build a program that loads the syscall number and arguments into a0.. via
// addi (immediates must fit a signed 12-bit field) and then executes ecall.
std::shared_ptr<const rv32::Program>
make_syscall_program(std::initializer_list<int32_t> arg_regs)
{
  std::vector<uint8_t> code;
  uint8_t reg = 10; // a0
  for (const int32_t imm : arg_regs) {
    append_u32(code, addi(reg, 0, imm));
    ++reg;
  }
  append_u32(code, ecall());
  auto result =
      rv32::Program::create(std::span<const uint8_t>(code.data(), code.size()));
  if (!result.has_value()) {
    ADD_FAILURE() << "failed to create test program";
    return {};
  }
  return std::make_shared<rv32::Program>(std::move(*result));
}

// NOLINTNEXTLINE(misc-multiple-inheritance)
class Rv32SyscallEntryBridgeTest : public ::testing::Test,
                                   public NativeContractTestFramework {
public:
  Rv32SyscallEntryBridgeTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  void SetUp() override { runtime_context_ = make_hog_exec_context(); }

  SystemCallHandler make_handler()
  {
    return make_new_system_call_handler(Transaction{}, *runtime_context_);
  }

  static std::unique_ptr<InvocationFrameTrace>
  start_frame(SystemCallHandler &handler)
  {
    auto stashed = handler.start_frame(
        Invocation{
            .invoked_address = make_contract_address(0),
            .entrypoint = ContractEntrypointID{},
            .gas_limit = kPlentyOfGas,
            .calldata = Calldata{},
        },
        std::to_underlying(InvokeFlag::None), ValidatedAuthTokens{});
    return std::move(stashed).value();
  }

  static void
  finish_frame(SystemCallHandler &handler,
               std::unique_ptr<InvocationFrameTrace> stashed_parent_trace,
               InvokeStatus<void> result = {})
  {
    auto status = handler.finish_frame(std::move(result),
                                       std::move(stashed_parent_trace))();
    ASSERT_TRUE(status.has_value());
  }

  static rv32::HostResult call_bridge(std::span<uint8_t> memory,
                                      SystemCallHandler &handler,
                                      uint64_t gas_remaining,
                                      const rv32::EcallArgs &args)
  {
    rv32::SyscallBridgeHost bridge(&handler, &syscall_entry);
    std::array<uint32_t, rv32::kRegisterCount> registers{};
    rv32::MachineView view(0, gas_remaining, registers, memory);
    return bridge.ecall(view, args);
  }

  std::unique_ptr<RuntimeContext> runtime_context_;
};

TEST_F(Rv32SyscallEntryBridgeTest, Sha256UsesRealSyscallEntry)
{
  std::vector<uint8_t> memory(256);
  constexpr std::string_view input = "rv32 syscall bridge";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  const std::array<uint32_t, 4> arg_words = {
      static_cast<uint32_t>(SyscallNo::kSHA256),
      kInputAddress,
      static_cast<uint32_t>(input.size()),
      kOutputAddress,
  };

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result =
      call_bridge(memory, handler, kPlentyOfGas, make_ecall_args(arg_words));
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(),
            static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeNone));
  EXPECT_GT(result.gas_consumed, 0U);
  const SHA256Hash expected = sha256(input);
  EXPECT_TRUE(
      std::ranges::equal(expected, std::span<const uint8_t>(memory).subspan(
                                       kOutputAddress, expected.size())));
}

TEST_F(Rv32SyscallEntryBridgeTest, MachineDebitsRealSha256SyscallGas)
{
  constexpr std::string_view input = "rv32 syscall bridge gas";
  const auto program = make_syscall_program({
      static_cast<int32_t>(SyscallNo::kSHA256),
      static_cast<int32_t>(kInputAddress),
      static_cast<int32_t>(input.size()),
      static_cast<int32_t>(kOutputAddress),
  });
  ASSERT_NE(program, nullptr);
  // Four argument loads plus the ecall instruction.
  constexpr uint64_t kSyscallInstructions = 5;

  rv32::Machine machine(program, 256, kPlentyOfGas);
  std::vector<uint8_t> memory(256);
  std::ranges::copy(input, memory.begin() + kInputAddress);
  ASSERT_TRUE(machine.view().write_memory(0, memory));

  auto handler = make_handler();
  rv32::SyscallBridgeHost bridge(&handler, &syscall_entry);
  auto stashed_trace = start_frame(handler);

  for (uint64_t i = 0; i + 1 < kSyscallInstructions; ++i) {
    EXPECT_EQ(machine.step(), rv32::StepResult::kContinue);
  }
  EXPECT_EQ(machine.step(), rv32::StepResult::kEcall);
  const auto pending_ecall = machine.pending_ecall();
  ASSERT_TRUE(pending_ecall.has_value());
  const uint64_t before_host = machine.gas_remaining();
  const rv32::HostResult host_result =
      bridge.ecall(machine.view(), pending_ecall.value_or(rv32::EcallArgs{}));
  ASSERT_EQ(host_result.status, rv32::HostStatus::kContinue);
  ASSERT_GT(host_result.gas_consumed, 0U);
  EXPECT_EQ(machine.complete_ecall(host_result), rv32::StepResult::kContinue);
  finish_frame(handler, std::move(stashed_trace));

  EXPECT_EQ(machine.gas_remaining(), before_host - host_result.gas_consumed);
  EXPECT_EQ(machine.gas_remaining(),
            kPlentyOfGas - (kSyscallInstructions * rv32::kBaseInstructionGas) -
                host_result.gas_consumed);
}

TEST_F(Rv32SyscallEntryBridgeTest, ParserFaultLeavesGuestMemoryUnchanged)
{
  std::vector<uint8_t> memory(256, 0xAA);
  constexpr std::string_view input = "fault";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  const std::array<uint32_t, 4> arg_words = {
      static_cast<uint32_t>(SyscallNo::kSHA256),
      kInputAddress,
      static_cast<uint32_t>(input.size()),
      static_cast<uint32_t>(memory.size() - 1),
  };
  const std::vector<uint8_t> before = memory;

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result =
      call_bridge(memory, handler, kPlentyOfGas, make_ecall_args(arg_words));
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(),
            static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeFault));
  EXPECT_EQ(memory, before);
}

TEST_F(Rv32SyscallEntryBridgeTest,
       DirectEntryParserFaultLeavesGuestMemoryUnchanged)
{
  std::vector<uint8_t> memory(256, 0xAA);
  constexpr std::string_view input = "fault";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  const std::vector<uint8_t> before = memory;

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  ExecutionContext context{
      .memory = std::as_writable_bytes(std::span<uint8_t>(memory)),
      .gas_remaining = kPlentyOfGas,
  };
  const auto result =
      syscall_entry(&handler, context,
                    static_cast<uint32_t>(SyscallNo::kSHA256), kInputAddress,
                    static_cast<uint32_t>(input.size()),
                    static_cast<uint32_t>(memory.size() - 1), 0, 0, 0, 0)();
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_TRUE(result.first.has_value());
  EXPECT_EQ(*result.first, SyscallErrorCode::kSyscallErrorCodeFault);
  EXPECT_EQ(memory, before);
}

TEST_F(Rv32SyscallEntryBridgeTest,
       InvokeOutputsPointerFaultLeavesGuestMemoryUnchanged)
{
  std::vector<uint8_t> memory(512, 0xBB);
  constexpr uint32_t kAddress = 128;
  constexpr uint32_t kInputs = 256;
  // Zero the address and CopyableInvokeInputs regions (flags == None) so the
  // only ABI fault is the deliberately out-of-bounds outputs pointer, which is
  // the final Invoke argument under the reduced struct-pointer ABI.
  std::ranges::fill(std::span<uint8_t>(memory).subspan(kAddress,
                                                       sizeof(ContractAddress)),
                    0);
  std::ranges::fill(
      std::span<uint8_t>(memory).subspan(kInputs, sizeof(CopyableInvokeInputs)),
      0);
  const auto bad_outputs = static_cast<uint32_t>(memory.size() - 1);
  const std::array<uint32_t, 8> arg_words = {
      static_cast<uint32_t>(SyscallNo::kInvoke),
      kAddress,    // address pointer
      kAddress,    // calldata pointer (zero length)
      0,           // calldata length
      0,           // owned auth token pointer
      0,           // owned auth token length
      kInputs,     // CopyableInvokeInputs pointer
      bad_outputs, // CopyableInvokeOutputs pointer
  };
  const std::vector<uint8_t> before = memory;

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result =
      call_bridge(memory, handler, kPlentyOfGas, make_ecall_args(arg_words));
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(),
            static_cast<uint32_t>(SyscallErrorCode::kSyscallErrorCodeFault));
  EXPECT_EQ(memory, before);
}

TEST_F(Rv32SyscallEntryBridgeTest, InvalidSyscallUsesNoSyscallFallback)
{
  std::vector<uint8_t> memory(256);
  const std::array<uint32_t, 1> arg_words = {
      static_cast<uint32_t>(kSyscallCount),
  };

  auto handler = make_handler();
  handler.enable_transaction_stats();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result =
      call_bridge(memory, handler, kPlentyOfGas, make_ecall_args(arg_words));
  finish_frame(handler, std::move(stashed_trace));

  ASSERT_EQ(result.status, rv32::HostStatus::kContinue);
  EXPECT_EQ(result.continue_value(),
            static_cast<uint32_t>(
                SyscallErrorCode::kSyscallErrorCodeNoSyscall));
  ASSERT_NE(handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(handler.get_transaction_stats()->invalid_syscall_count, 1);
}

TEST_F(Rv32SyscallEntryBridgeTest, OutOfGasDoesNotWriteBackGuestMemory)
{
  std::vector<uint8_t> memory(256, 0xCC);
  constexpr std::string_view input = "out of gas";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  const std::array<uint32_t, 4> arg_words = {
      static_cast<uint32_t>(SyscallNo::kSHA256),
      kInputAddress,
      static_cast<uint32_t>(input.size()),
      kOutputAddress,
  };
  const std::vector<uint8_t> before = memory;

  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  rv32::HostResult result = call_bridge(memory, handler, kSyscallBaseCost + 1,
                                        make_ecall_args(arg_words));
  finish_frame(handler, std::move(stashed_trace),
               std::unexpected(InvokeError{InvokeErrorCode::OutOfGas}));

  EXPECT_EQ(result.status, rv32::HostStatus::kOutOfGas);
  EXPECT_EQ(memory, before);
}

TEST_F(Rv32SyscallEntryBridgeTest, DirectEntryOutOfGasWritesLiveOutputSpan)
{
  // Rv32Runtime::invoke_internal calls syscall_entry with ExecutionContext
  // memory backed directly by ThreadedMachine memory; unlike
  // SyscallBridgeHost, there is no scratch copy between output spans and guest
  // memory.
  std::vector<uint8_t> memory(256, 0xCC);
  constexpr std::string_view input = "out of gas";
  std::ranges::copy(input, memory.begin() + kInputAddress);
  const std::vector<uint8_t> before = memory;

  // Afford entry so SHA256 runs, but leave too little gas for its body cost.
  constexpr GasAmount kBudget = kSyscallBaseCost + 1;
  auto handler = make_handler();
  auto stashed_trace = start_frame(handler);
  ExecutionContext context{
      .memory = std::as_writable_bytes(std::span<uint8_t>(memory)),
      .gas_remaining = kBudget,
  };
  const auto result =
      syscall_entry(&handler, context,
                    static_cast<uint32_t>(SyscallNo::kSHA256), kInputAddress,
                    static_cast<uint32_t>(input.size()), kOutputAddress, 0, 0,
                    0, 0)();
  finish_frame(handler, std::move(stashed_trace),
               std::unexpected(InvokeError{InvokeErrorCode::OutOfGas}));

  ASSERT_FALSE(result.first.has_value());
  EXPECT_EQ(result.first.error().code(), HostFnErrorCode::OutOfGas);
  EXPECT_EQ(result.second, kBudget);
  // Once entry is affordable, SHA256 still writes its live output before
  // the final accounting rejects its body charge.
  EXPECT_NE(memory, before);
  const SHA256Hash expected = sha256(input);
  EXPECT_TRUE(
      std::ranges::equal(expected, std::span<const uint8_t>(memory).subspan(
                                       kOutputAddress, expected.size())));
}

TEST_F(Rv32SyscallEntryBridgeTest,
       InterpretersAgreeOnObservableExactBudgetOutOfGas)
{
  // At an exact gas boundary the SHA256 syscall body runs but is then charged
  // into OutOfGas. The canonical per-instruction Machine runs the syscall
  // against a scratch buffer (discarded on OutOfGas) while the threaded
  // interpreter runs it against live guest memory (which retains the staged
  // output) -- so the two interpreters' final guest memory legitimately
  // differs. That memory is frame-local state a trap discards (see
  // threaded_machine.h); what consensus observes is the (trap classification,
  // gas_remaining) pair, and this test pins that both interpreters agree on it.
  // The divergence in the discarded frame memory is therefore WAI, not a bug
  // (issue #3780): after OutOfGas the whole frame is thrown away.
  constexpr std::string_view input = "out of gas";
  // Cover the four argument loads, ecall, crossing, and SHA256 body exactly.
  // The body runs, then the inclusive final gas check reports OutOfGas.
  constexpr uint64_t kExactBudget = (5 * rv32::kBaseInstructionGas) +
                                    kSyscallBaseCost +
                                    GasCost::sha256_hash(input.size());

  std::vector<uint8_t> code;
  uint8_t reg = 10; // a0
  for (const int32_t imm :
       {static_cast<int32_t>(SyscallNo::kSHA256),
        static_cast<int32_t>(kInputAddress), static_cast<int32_t>(input.size()),
        static_cast<int32_t>(kOutputAddress)}) {
    append_u32(code, addi(reg, 0, imm));
    ++reg;
  }
  append_u32(code, ecall());

  std::vector<uint8_t> initial_memory(256, 0xCC);
  std::ranges::copy(input, initial_memory.begin() + kInputAddress);

  // Canonical per-instruction Machine driven through the scratch-buffer bridge.
  auto canonical_program =
      rv32::Program::create(std::span<const uint8_t>(code.data(), code.size()));
  ASSERT_TRUE(canonical_program.has_value());
  rv32::Machine canonical(std::make_shared<rv32::Program>(
                              std::move(*canonical_program)),
                          initial_memory.size(), kExactBudget);
  ASSERT_TRUE(canonical.view().write_memory(0, initial_memory));
  auto canonical_handler = make_handler();
  rv32::SyscallBridgeHost canonical_bridge(&canonical_handler, &syscall_entry);
  auto canonical_trace = start_frame(canonical_handler);
  const rv32::StepResult canonical_result = canonical.run(canonical_bridge);
  finish_frame(canonical_handler, std::move(canonical_trace),
               std::unexpected(InvokeError{InvokeErrorCode::OutOfGas}));

  // Threaded interpreter driven against live guest memory, exactly as
  // Rv32Runtime::invoke_internal does.
  auto pre_instantiation = rv32::ThreadedMachinePreInstantiation::create(
      std::span<const uint8_t>(code.data(), code.size()), initial_memory, 0,
      initial_memory.size(), false);
  ASSERT_TRUE(pre_instantiation.has_value());
  rv32::ThreadedMachine threaded(*pre_instantiation, kExactBudget);
  auto threaded_handler = make_handler();
  auto threaded_trace = start_frame(threaded_handler);
  ASSERT_EQ(threaded.start(), rv32::StepResult::kEcall);
  ASSERT_TRUE(threaded.pending_ecall().has_value());
  const rv32::EcallArgs ecall =
      threaded.pending_ecall().value_or(rv32::EcallArgs{});
  ExecutionContext threaded_context{
      .memory = std::as_writable_bytes(threaded.memory()),
      .gas_remaining = threaded.gas_remaining(),
  };
  const auto threaded_syscall =
      syscall_entry(&threaded_handler, threaded_context, ecall.a0, ecall.a1,
                    ecall.a2, ecall.a3, ecall.a4, ecall.a5, ecall.a6,
                    ecall.a7)();
  const rv32::StepResult threaded_result = threaded.resume_from_ecall(
      rv32::HostResult::out_of_gas(threaded_syscall.second));
  finish_frame(threaded_handler, std::move(threaded_trace),
               std::unexpected(InvokeError{InvokeErrorCode::OutOfGas}));

  // The consensus-visible result is identical on both interpreters.
  EXPECT_EQ(canonical_result, rv32::StepResult::kTrap);
  EXPECT_EQ(canonical.trap(), rv32::Trap::kOutOfGas);
  EXPECT_EQ(canonical.gas_remaining(), 0U);
  EXPECT_EQ(threaded_result, rv32::StepResult::kTrap);
  EXPECT_EQ(threaded.trap(), rv32::Trap::kOutOfGas);
  EXPECT_EQ(threaded.gas_remaining(), 0U);
}

TEST_F(Rv32SyscallEntryBridgeTest, MachineRunCompletesRealReturnSyscall)
{
  // Return with an empty buffer and no owned auth tokens: a0 = kReturn and the
  // remaining argument registers default to zero (empty spans, revert false).
  const auto program =
      make_syscall_program({static_cast<int32_t>(SyscallNo::kReturn)});
  ASSERT_NE(program, nullptr);

  rv32::Machine machine(program, 256, kPlentyOfGas);

  auto handler = make_handler();
  rv32::SyscallBridgeHost bridge(&handler, &syscall_entry);
  auto stashed_trace = start_frame(handler);
  const rv32::StepResult result = machine.run(bridge);
  finish_frame(handler, std::move(stashed_trace));

  EXPECT_EQ(result, rv32::StepResult::kHalted);
  EXPECT_TRUE(machine.halted());
  EXPECT_EQ(machine.halt_code(), 0U);
}

} // namespace
} // namespace hogd

// NOLINTEND(readability-identifier-naming)
