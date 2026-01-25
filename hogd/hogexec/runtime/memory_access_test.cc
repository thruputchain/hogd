#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm_test_framework.h"

namespace hogd {

class MemoryAccessTest : public ::testing::TestWithParam<WasmEngine>,
                         public WasmTestFramework {
protected:
  void SetUp() override
  {
    setup_wasm_context(GetParam());
    Contract contract(ContractType::kWasmi);
    // this wasm just invokes an external call.  We're just reusing it here.
    contract.wasmi_contract() =
        test::load_wasm_from_file("test_set_memory.wasm");
    auto deployed_contract_hash = DeployedContractHash{
        .hash = sha256(contract),
        .contract_size =
            static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
        .contract_type = ContractType::kWasmi,
    };
    make_syscall_handler(contract);
    auto runtime_res =
        get_context()->make_new_runtime(deployed_contract_hash,
                                        get_syscall_handler(), true);
    ASSERT_TRUE(runtime_res.has_value());
    runtime_ = std::move(runtime_res.value());

    ASSERT_TRUE(!!runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

TEST_P(MemoryAccessTest, ReadWrites)
{
  // assert memory exists
  auto res = runtime_->invoke("size", UINT64_MAX);
  EXPECT_TRUE(res.is_root_success());

  res = runtime_->invoke("store", UINT64_MAX);

  EXPECT_TRUE(res.is_root_success());

  // This API for accessing memory directly is not particularly convenient
  // and may be bug prone (i.e. alignment errors, potentally)
  // but it suffices for basic tests.

  auto mem = runtime_->get_memory();

  ASSERT_FALSE(mem.empty());

  // magic constants set in wat/test_set_memory.wat
  uint32_t read_val = *reinterpret_cast<uint32_t *>((mem.data() + 12));
  EXPECT_EQ(read_val, 0xCC);

  read_val = *reinterpret_cast<uint32_t *>((mem.data() + 44));
  EXPECT_EQ(read_val, 0xFFAABBCC);

  *reinterpret_cast<uint32_t *>((mem.data()) + 32) = 0xABCDEF01;

  res = runtime_->invoke("load", UINT64_MAX);
  ASSERT_TRUE(res.result_);

  // checks that the loaded result is 0xABCDEF01
  // EXPECT_EQ(*res.result_, 0xABCDEF01);
  res = runtime_->invoke("load_expect", UINT64_MAX);
  ASSERT_TRUE(res.result_);
}

INSTANTIATE_TEST_SUITE_P(WasmEngines, MemoryAccessTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
