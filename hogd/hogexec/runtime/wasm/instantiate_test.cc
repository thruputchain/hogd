#include <gtest/gtest.h>

#include <stdexcept>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"

namespace hogd {

class InstantiateTest : public ::testing::TestWithParam<WasmEngine>,
                        public WasmTestFramework {
protected:
  void SetUp() override { setup_wasm_context(GetParam()); }
};

TEST_P(InstantiateTest, MissingImportFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };

  make_syscall_handler(contract);
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

TEST_P(InstantiateTest, InstantiateFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_fail.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

TEST_P(InstantiateTest, InstantiateUnexportedMemory)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_unexported_memory.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

// Modules with a start section are now rejected at instrumentation time
// by wasm_meter (which cannot set the stack budget before start runs).
TEST_P(InstantiateTest, InstantiateNoStart)
{
  EXPECT_THROW(test::load_wasm_from_file("test_no_start.wasm"),
               std::runtime_error);
}

// Regression test for audit issue #1: instantiation of a module with an
// out-of-bounds data segment should return a null runtime, not crash.
// The data segment is at offset 70000, beyond 1-page (65536 byte) memory.
TEST_P(InstantiateTest, InstantiateOobDataSegment)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_oob_data_segment.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

// Regression test for audit issue #1 variant: active element segment
// initialization with an out-of-bounds table offset should return a null
// runtime, not crash.
TEST_P(InstantiateTest, InstantiateOobElementSegment)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_oob_element_segment.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

INSTANTIATE_TEST_SUITE_P(WasmEngines, InstantiateTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
