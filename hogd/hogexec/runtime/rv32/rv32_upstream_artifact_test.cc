#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/artifact.h"
#include "hogd/hogexec/runtime/rv32/dispatch.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/runtime_context.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "hogd/hogexec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hogexec/runtime/rv32/test_program_builder.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

// Test fixture combines gtest with the existing native-contract harness.
// NOLINTBEGIN(misc-multiple-inheritance)

namespace hogd {
namespace {

using rv32::test_program::addi;
using rv32::test_program::branch;
using rv32::test_program::j_type;
using rv32::test_program::jalr;
using rv32::test_program::load;
using rv32::test_program::op;
using rv32::test_program::store;

constexpr int32_t kRv32ContractId = 7;
constexpr ContractAddress kRv32ContractAddress =
    NativeContractTestFramework::make_contract_address(7);
constexpr uint32_t kFrameAddress = 64;
constexpr uint32_t kReturnBufferAddress = 192;
constexpr uint32_t kDataAddress = 256;
constexpr uint32_t kMemoryBytes = 384;
constexpr GasAmount kEnoughGas = 10'000;

void
store_le64(std::span<uint8_t> bytes, uint32_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
store_le32(std::span<uint8_t> bytes, uint32_t offset, uint32_t value)
{
  for (size_t i = 0; i < sizeof(uint32_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

std::vector<uint8_t>
make_return_memory()
{
  std::vector<uint8_t> memory(kMemoryBytes);
  memory[kReturnBufferAddress] = 0xffU;
  const std::array<uint64_t, 6> frame_words = {
      static_cast<uint64_t>(SyscallNo::kReturn),
      kReturnBufferAddress,
      1,
      0,
      0,
      0,
  };
  for (size_t i = 0; i < frame_words.size(); ++i) {
    store_le64(memory, kFrameAddress + (i * sizeof(uint64_t)), frame_words[i]);
  }
  return memory;
}

std::vector<uint32_t>
return_with_code(uint8_t code)
{
  return {
      addi(7, 0, code),
      addi(8, 0, kReturnBufferAddress),
      store(8, 7, 0, 0),
      addi(10, 0, kFrameAddress),
      addi(17, 0, rv32::kSyscallBridgeVersion),
      0x00000073U,
  };
}

std::vector<uint32_t>
fail_deterministically()
{
  return {
      // Jumping to an unaligned PC is a deterministic guest failure at the
      // runtime boundary, mirroring an upstream fail label.
      j_type(0, 2),
  };
}

std::vector<uint32_t>
wrap_with_pass_fail(std::vector<uint32_t> setup)
{
  std::vector<uint32_t> words = std::move(setup);
  constexpr size_t kReturnWords = 6;
  constexpr auto kBranchOverPassReturn =
      static_cast<int32_t>((1 + kReturnWords) * rv32::kInstructionBytes);
  words.push_back(branch(1, 5, 6, kBranchOverPassReturn));

  std::vector<uint32_t> pass = return_with_code(0);
  words.insert(words.end(), pass.begin(), pass.end());

  std::vector<uint32_t> fail = fail_deterministically();
  words.insert(words.end(), fail.begin(), fail.end());
  return words;
}

rv32::RuntimeContractImage
make_artifact_image(std::span<const uint32_t> words,
                    std::vector<uint8_t> initial_memory)
{
  const std::array<rv32::DispatchEntry, 1> dispatch = {
      rv32::DispatchEntry{.method_id = 0, .pc = 0}};
  const std::vector<uint8_t> artifact =
      rv32::test_artifact::build_artifact(words, initial_memory, dispatch);
  auto parsed = rv32::parse_artifact(artifact);
  if (!parsed.has_value()) {
    ADD_FAILURE() << "failed to parse upstream-derived RV32 artifact";
    return {};
  }
  return rv32::RuntimeContractImage{
      .program = std::move(parsed->program),
      .initial_memory = std::move(parsed->initial_memory),
      .dispatch_table = std::move(parsed->dispatch_table),
      .memory_bytes = parsed->memory_bytes,
  };
}

struct UpstreamArtifactCase {
  std::string name;
  std::vector<uint32_t> words;
  std::vector<uint8_t> memory;
};

std::vector<UpstreamArtifactCase>
upstream_artifact_cases()
{
  std::vector<uint8_t> lw_memory = make_return_memory();
  store_le32(lw_memory, kDataAddress, 42);

  return {
      {.name = "synthetic_rv32ui_simple",
       .words = wrap_with_pass_fail({addi(5, 0, 1), addi(6, 0, 1)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32ui_add",
       .words = wrap_with_pass_fail(
           {addi(1, 0, 7), addi(2, 0, 8), op(5, 1, 2, 0, 0), addi(6, 0, 15)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32ui_addi",
       .words =
           wrap_with_pass_fail({addi(1, 0, 10), addi(5, 1, -3), addi(6, 0, 7)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32ui_beq",
       .words = wrap_with_pass_fail({addi(1, 0, 5), addi(2, 0, 5),
                                     addi(5, 0, 1), branch(0, 1, 2, 8),
                                     addi(5, 0, 9), addi(6, 0, 1)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32ui_jal",
       .words =
           wrap_with_pass_fail({j_type(5, 8), addi(5, 0, 99), addi(6, 0, 4)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32ui_jalr",
       .words = wrap_with_pass_fail(
           {addi(1, 0, 13), jalr(5, 1, -1), addi(5, 0, 99), addi(6, 0, 8)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32ui_lw",
       .words = wrap_with_pass_fail(
           {addi(1, 0, kDataAddress), load(5, 2, 1, 0), addi(6, 0, 42)}),
       .memory = std::move(lw_memory)},
      {.name = "synthetic_rv32ui_sw",
       .words = wrap_with_pass_fail({addi(1, 0, kDataAddress), addi(2, 0, 37),
                                     store(1, 2, 0, 2), load(5, 2, 1, 0),
                                     addi(6, 0, 37)}),
       .memory = make_return_memory()},
      {.name = "synthetic_rv32um_mul",
       .words = wrap_with_pass_fail(
           {addi(1, 0, 7), addi(2, 0, 6), op(5, 1, 2, 0, 1), addi(6, 0, 42)}),
       .memory = make_return_memory()},
  };
}

std::shared_ptr<const Contract>
make_rv32_contract_xdr()
{
  auto contract = std::make_shared<Contract>(ContractType::kMockTest);
  contract->mock_contract_id() = kRv32ContractId;
  return contract;
}

Transaction
make_root_transaction(GasAmount gas_limit)
{
  return Transaction{
      .invocation =
          Invocation{
              .invoked_address = kRv32ContractAddress,
              .entrypoint = entrypoint_from_method_id(0),
              .gas_limit = gas_limit,
              .calldata = Calldata{},
          },
      .uniquifier =
          Uniquifier{
              .block_expiry_height = std::numeric_limits<BlockHeight>::max(),
              .nonce = 1,
          },
  };
}

class Rv32UpstreamArtifactTest
  : public ::testing::TestWithParam<UpstreamArtifactCase>,
    public NativeContractTestFramework {
public:
  Rv32UpstreamArtifactTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}

protected:
  void install_contract(std::shared_ptr<const Contract> const &contract)
  {
    write_contract(contract);
    write_deployment(kRv32ContractAddress, contract);
    setup_grant_storage_token(kRv32ContractAddress, 1'000'000'000);
  }
};

TEST_P(Rv32UpstreamArtifactTest, passesThroughRuntimeContext)
{
  const UpstreamArtifactCase &test_case = GetParam();
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId,
        make_artifact_image(test_case.words, test_case.memory)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  handler.enable_tracing();

  const auto result = handler.root_invoke();

  ASSERT_TRUE(result.is_root_success()) << test_case.name;
  const auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return)
      << test_case.name;
  ASSERT_EQ(output.return_buffer().size(), 1U) << test_case.name;
  EXPECT_EQ(output.return_buffer()[0], 0U) << test_case.name;
  ASSERT_NE(handler.get_trace(), nullptr);
  ASSERT_EQ(handler.get_trace()->system_calls.size(), 1U) << test_case.name;
  EXPECT_EQ(handler.get_trace()->system_calls[0].body.syscall_number(),
            SyscallNo::kReturn)
      << test_case.name;
}

TEST_F(Rv32UpstreamArtifactTest, failPathMapsToDeterministicRuntimeFailure)
{
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId,
        make_artifact_image(wrap_with_pass_fail({addi(5, 0, 1), addi(6, 0, 2)}),
                            make_return_memory())}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  handler.enable_tracing();

  const auto result = handler.root_invoke();

  ASSERT_FALSE(result.is_root_success());
  EXPECT_EQ(result.result_.error().code(), InvokeErrorCode::Deterministic);
  ASSERT_NE(handler.get_trace(), nullptr);
  EXPECT_TRUE(handler.get_trace()->system_calls.empty());
}

INSTANTIATE_TEST_SUITE_P(
    SyntheticRiscvTestShapes, Rv32UpstreamArtifactTest,
    ::testing::ValuesIn(upstream_artifact_cases()),
    [](const testing::TestParamInfo<UpstreamArtifactCase> &info) {
      return info.param.name;
    });

} // namespace
} // namespace hogd

// NOLINTEND(misc-multiple-inheritance)
