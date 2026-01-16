#include <gtest/gtest.h>

#include "address_auth.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm_test_framework.h"
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
                                                 get_syscall_handler(), true);
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
                                                 get_syscall_handler(), true);
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
                                                 get_syscall_handler(), true);
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

TEST_P(InstantiateTest, InstantiateNoStart)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_no_start.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash,
                                                 get_syscall_handler(), true);
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
