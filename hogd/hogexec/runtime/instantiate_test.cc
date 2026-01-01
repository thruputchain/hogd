#include <gtest/gtest.h>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm_test_framework.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "hogd/hogexec/runtime/wasmtime_runtime.h"

namespace hogd {

class InstantiateTest : public ::testing::Test, public WasmTestFramework {
protected:
  WasmtimeGlobalContext wasmtime_global_context_ = WasmtimeGlobalContext(1);
  WasmtimeContext wasmtime_context_ = WasmtimeContext(wasmtime_global_context_);
};

TEST_F(InstantiateTest, MissingImportFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");
  make_syscall_handler(contract);
  auto deployed_contract_hash = DeployedContractHash{
      .hash = sha256(contract),
      .contract_size =
          static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
      .contract_type = ContractType::kWasmi,
  };
  auto runtime =
      wasmtime_context_.make_new_runtime(deployed_contract_hash,
                                         syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

TEST_F(InstantiateTest, InstantiateFail)
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
  auto runtime =
      wasmtime_context_.make_new_runtime(deployed_contract_hash,
                                         syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

TEST_F(InstantiateTest, InstantiateUnexportedMemory)
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
  auto runtime =
      wasmtime_context_.make_new_runtime(deployed_contract_hash,
                                         syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());

  EXPECT_FALSE(runtime.value());
}

TEST_F(InstantiateTest, InstantiateNoStart)
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
  auto runtime =
      wasmtime_context_.make_new_runtime(deployed_contract_hash,
                                         syscall_handler_.get(), true);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_FALSE(runtime.value());
}

} // namespace hogd
