#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

class MemoryAccessTest : public ::testing::TestWithParam<RuntimeEngine> {
protected:
  void SetUp() override
  {
    auto c = test::load_wasm_from_file("test_set_memory.wasm");
    auto context = make_runtime_context(GetParam());
    runtime_ = context->make_new_runtime(c, nullptr);

    ASSERT_TRUE(runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

TEST_P(MemoryAccessTest, ReadWrites)
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

INSTANTIATE_TEST_SUITE_P(AllEngines, MemoryAccessTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });
} // namespace hogd
