#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"

namespace hogd {

class InstantiateTest : public ::testing::TestWithParam<RuntimeEngine> {
protected:
};

TEST_P(InstantiateTest, MissingImportFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");

  auto context = make_runtime_context(GetParam());
  auto runtime = context->make_new_runtime(contract, nullptr, true);
  EXPECT_EQ(!engine_has_eager_link_guards(GetParam()), !!runtime);
}

// fails if memory is too big
TEST_P(InstantiateTest, InstantiateFail)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_fail.wasm");
  auto context = make_runtime_context(GetParam());
  auto runtime = context->make_new_runtime(contract, nullptr, true);
  EXPECT_FALSE(runtime);
}

TEST_P(InstantiateTest, InstantiateUnexportedMemory)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() =
      test::load_wasm_from_file("test_instantiate_unexported_memory.wasm");
  auto context = make_runtime_context(GetParam());
  auto runtime = context->make_new_runtime(contract, nullptr, true);
  EXPECT_FALSE(runtime);
}

INSTANTIATE_TEST_SUITE_P(AllEngines, InstantiateTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });

} // namespace hogd
