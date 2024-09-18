#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/runtime/status.h"
#include "hogd/runtime/test_load_wasm.h"
#include "hogd/runtime/wasmi_runtime.h"

#include <cstdint>
#include <memory>

namespace hogd {

class MemoryAccessTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // this wasm just invokes an external call.  We're just reusing it here.
    auto c = test::load_wasm_from_file("test_set_memory.wasm");
    WasmiContext context;
    runtime_ = context.make_new_runtime(c, nullptr);
    ASSERT_TRUE(!!runtime_);
  }

  std::unique_ptr<WasmiRuntime> runtime_;
};

TEST_F(MemoryAccessTest, ReadWrites)
{
  // assert memory exists
  auto res = runtime_->invoke("size", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 1);

  res = runtime_->invoke("store", UINT64_MAX);

  EXPECT_TRUE(!!res.result);

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

  ASSERT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 0xABCDEF01);
}

} // namespace hogd
