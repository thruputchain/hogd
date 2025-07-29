#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

TEST(WasmRuntimeTest, MissingImportFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");
  WasmiContext context;
  auto runtime =
      context.make_new_runtime(contract, sha256(contract), nullptr, true);

  EXPECT_FALSE(runtime);
}

TEST(WasmRuntimeTest, InstantiateFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_fail.wasm");
  WasmiContext context;
  auto runtime =
      context.make_new_runtime(contract, sha256(contract), nullptr, true);
  EXPECT_FALSE(runtime);
}

TEST(WasmRuntimeTest, InstantiateUnexportedMemory)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_unexported_memory.wasm");
  WasmiContext context;
  auto runtime =
      context.make_new_runtime(contract, sha256(contract), nullptr, true);
  EXPECT_FALSE(runtime);
}

} // namespace hogd
