#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "built_contracts/no_syscall_test.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/rv32/program.h"
#include "hogd/hogexec/runtime/rv32/runtime_context.h"
#include "hogd/hogexec/runtime/rv32/syscall_bridge.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
namespace {

constexpr int32_t kRv32ContractId = 7;
constexpr ContractAddress kWasmContractAddress =
    NativeContractTestFramework::make_contract_address(6);
constexpr ContractAddress kRv32ContractAddress =
    NativeContractTestFramework::make_contract_address(7);
constexpr uint32_t kFrameAddress = 64;
constexpr uint32_t kReturnBufferAddress = 192;
constexpr GasAmount kEnoughGas = 1000;

void
append_u32(std::vector<uint8_t> &code, uint32_t word)
{
  code.push_back(static_cast<uint8_t>(word));
  code.push_back(static_cast<uint8_t>(word >> 8));
  code.push_back(static_cast<uint8_t>(word >> 16));
  code.push_back(static_cast<uint8_t>(word >> 24));
}

std::shared_ptr<const rv32::Program>
make_program(std::initializer_list<uint32_t> words)
{
  std::vector<uint8_t> code;
  code.reserve(words.size() * rv32::kInstructionBytes);
  for (const uint32_t word : words) {
    append_u32(code, word);
  }
  auto result =
      rv32::Program::create(std::span<const uint8_t>(code.data(), code.size()));
  if (!result.has_value()) {
    ADD_FAILURE() << "failed to create RV32 test program";
    return {};
  }
  return std::make_shared<rv32::Program>(std::move(*result));
}

uint32_t
i_type(uint32_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm)
{
  return (uint32_t(imm) & 0xfffU) << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | uint32_t{rd} << 7 | opcode;
}

uint32_t
addi(uint8_t rd, uint8_t rs1, int32_t imm)
{
  return i_type(0x13, rd, 0, rs1, imm);
}

uint32_t
s_type(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm)
{
  const uint32_t uimm = uint32_t(imm) & 0xfffU;
  return (uimm >> 5) << 25 | uint32_t{rs2} << 20 | uint32_t{rs1} << 15 |
         uint32_t{funct3} << 12 | (uimm & 0x1fU) << 7 | 0x23U;
}

uint32_t
sw(uint8_t rs2, uint8_t rs1, int32_t imm)
{
  return s_type(2, rs1, rs2, imm);
}

uint32_t
ecall()
{
  return 0x00000073U;
}

void
store_le64(std::span<uint8_t> bytes, uint32_t offset, uint64_t value)
{
  for (size_t i = 0; i < sizeof(uint64_t); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void
write_return_frame(std::span<uint8_t> memory, std::span<const uint8_t> payload)
{
  const std::array<uint64_t, 6> frame_words = {
      static_cast<uint64_t>(SyscallNo::kReturn),
      kReturnBufferAddress,
      payload.size(),
      0,
      0,
      0,
  };
  for (size_t i = 0; i < frame_words.size(); ++i) {
    store_le64(memory, kFrameAddress + i * sizeof(uint64_t), frame_words[i]);
  }
  std::ranges::copy(payload, memory.begin() + kReturnBufferAddress);
}

std::vector<uint8_t>
make_initial_return_memory(std::span<const uint8_t> payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  write_return_frame(initial_memory, payload);
  return initial_memory;
}

rv32::RuntimeContractImage
make_return_image(std::span<const uint8_t> payload)
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory = make_initial_return_memory(payload);
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
  };
}

rv32::RuntimeContractImage
make_invalid_syscall_then_return_image()
{
  constexpr uint32_t kMemoryBytes = 256;
  std::vector<uint8_t> initial_memory(kMemoryBytes);
  store_le64(initial_memory, kFrameAddress,
             static_cast<uint64_t>(kSyscallCount));
  return rv32::RuntimeContractImage{
      .program = make_program({
          addi(10, 0, kFrameAddress),
          addi(8, 0, kFrameAddress),
          addi(17, 0, rv32::kSyscallBridgeVersion),
          ecall(),
          addi(5, 0, std::to_underlying(SyscallNo::kReturn)),
          sw(5, 8, 0),
          addi(10, 8, 0),
          ecall(),
      }),
      .initial_memory = std::move(initial_memory),
      .memory_bytes = kMemoryBytes,
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
              .entrypoint = ContractEntrypointID{},
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

class Rv32RuntimeContextTest : public ::testing::Test,
                               public NativeContractTestFramework {
public:
  Rv32RuntimeContextTest()
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

TEST_F(Rv32RuntimeContextTest, InvalidSyscallReturnMatchesWasmStatusAndTrace)
{
  auto wasm_context = make_hogexec_context();
  auto wasm_contract =
      ContractTraitMeta<no_syscall_test::NoSyscallTest>::load_wasm();
  write_contract(wasm_contract);
  write_deployment(kWasmContractAddress, wasm_contract);
  setup_grant_storage_token(kWasmContractAddress, 1'000'000'000);
  auto rv32_context = rv32::RuntimeContext(
      {{kRv32ContractId, make_invalid_syscall_then_return_image()}});
  auto rv32_contract = make_rv32_contract_xdr();
  install_contract(rv32_contract);

  ContractTraitClient<no_syscall_test::NoSyscallTest> wasm_client(
      kWasmContractAddress);
  auto wasm_handler = make_new_system_call_handler(
      Transaction{.invocation = wasm_client.make_invocation<"run_test">(
                      kEnoughGas, static_cast<uint64_t>(kSyscallCount)),
                  .uniquifier =
                      Uniquifier{
                          .block_expiry_height =
                              std::numeric_limits<BlockHeight>::max(),
                          .nonce = 1,
                      }},
      *wasm_context);
  wasm_handler.enable_tracing();
  wasm_handler.enable_transaction_stats();

  auto rv32_handler =
      make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                   rv32_context);
  rv32_handler.enable_tracing();
  rv32_handler.enable_transaction_stats();

  const auto wasm_result = wasm_handler.root_invoke();
  const auto rv32_result = rv32_handler.root_invoke();

  ASSERT_TRUE(wasm_result.is_root_success());
  ASSERT_TRUE(rv32_result.is_root_success());

  const auto wasm_output = wasm_handler.get_most_recent_frame_output();
  const auto rv32_output = rv32_handler.get_most_recent_frame_output();
  ASSERT_EQ(wasm_output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(rv32_output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(wasm_output.return_buffer().size(), 0U);
  EXPECT_EQ(rv32_output.return_buffer().size(), 0U);

  ASSERT_NE(wasm_handler.get_trace(), nullptr);
  ASSERT_NE(rv32_handler.get_trace(), nullptr);
  EXPECT_EQ(wasm_handler.get_trace()->status.status_code(),
            rv32_handler.get_trace()->status.status_code());
  ASSERT_NE(wasm_handler.get_transaction_stats(), nullptr);
  ASSERT_NE(rv32_handler.get_transaction_stats(), nullptr);
  EXPECT_EQ(wasm_handler.get_transaction_stats()->invalid_syscall_count, 1);
  EXPECT_EQ(rv32_handler.get_transaction_stats()->invalid_syscall_count, 1);
}

TEST_F(Rv32RuntimeContextTest, RootInvokeReturnsBufferThroughSystemCallHandler)
{
  const std::array<uint8_t, 4> payload = {'r', 'v', '3', '2'};
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, make_return_image(payload)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  auto result = handler.root_invoke();

  ASSERT_TRUE(result.is_root_success());
  EXPECT_EQ(result.gas_consumed_, 3U * rv32::kBaseInstructionGas);
  auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  ASSERT_EQ(output.return_buffer().size(), payload.size());
  EXPECT_TRUE(std::ranges::equal(output.return_buffer(), payload));
}

TEST_F(Rv32RuntimeContextTest, DirectInvokeNormalizesOutOfGasConsumption)
{
  const std::array<uint8_t, 0> payload = {};
  auto rv32_context =
      rv32::RuntimeContext({{kRv32ContractId, make_return_image(payload)}});
  auto contract = make_rv32_contract_xdr();
  install_contract(contract);

  auto handler = make_new_system_call_handler(make_root_transaction(kEnoughGas),
                                              rv32_context);
  const DeployedContractHash deployed =
      NativeContractTestFramework::make_deployed_contract_hash(contract);
  auto runtime = rv32_context.make_new_runtime(deployed, &handler);
  ASSERT_TRUE(runtime.has_value());
  ASSERT_NE(runtime.value(), nullptr);

  auto result = runtime.value()->invoke(0, 2);

  ASSERT_FALSE(result.is_root_success());
  ASSERT_EQ(result.result_.error().code(), InvokeErrorCode::OutOfGas);
  EXPECT_EQ(result.gas_consumed_, 2U);
}

TEST_F(Rv32RuntimeContextTest, VerifyRejectsUndefinedRv32MockContract)
{
  auto rv32_context = rv32::RuntimeContext({});
  auto contract = make_rv32_contract_xdr();

  EXPECT_EQ(rv32_context.verify_contract(*contract),
            InvokeErrorCode::Deterministic);
}

} // namespace
} // namespace hogd
