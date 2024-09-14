#include <gtest/gtest.h>

#include "hogd/runtime/wasmi_runtime.h"

#include "hogd/runtime/test_load_wasm.h"

namespace hogd {

TEST(WasmRuntimeTest, MissingImportFail)
{
  auto c = test::load_wasm_from_file("test_invoke.wasm");
  WasmiContext context;
  auto runtime = context.make_new_runtime(c, nullptr);

  EXPECT_FALSE(!!runtime);
}

} // namespace hogd
