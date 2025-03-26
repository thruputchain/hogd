#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"

namespace hogd {

class InstantiateTest : public ::testing::TestWithParam<RuntimeEngine> {
protected:
};

TEST_P(InstantiateTest, MissingImportFail)
{
  auto c = test::load_wasm_from_file("test_invoke.wasm");
  auto context = make_runtime_context(GetParam());
  auto runtime = context->make_new_runtime(c, nullptr, true);

  if (engine_has_eager_link_guards(GetParam())) {
    EXPECT_FALSE(runtime);
  }
  else {
    EXPECT_TRUE(runtime);
  }
}

INSTANTIATE_TEST_SUITE_P(AllEngines, InstantiateTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });

} // namespace hogd
