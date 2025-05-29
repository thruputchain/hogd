#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

class MemoryAccessTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    Contract contract(ContractType::kWasmi);
    // this wasm just invokes an external call.  We're just reusing it here.
    contract.wasmi_contract() =
        test::load_wasm_from_file("test_set_memory.wasm");
    WasmiContext context;
    runtime_ = context.make_new_runtime(contract, nullptr, true);
    ASSERT_TRUE(!!runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

TEST_F(MemoryAccessTest, ReadWrites)
{
  // assert memory exists
  auto res = runtime_->invoke("size", UINT64_MAX);
  EXPECT_TRUE(res.result_);

  res = runtime_->invoke("store", UINT64_MAX);

  EXPECT_TRUE(res.result_);

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

} // namespace hogd
