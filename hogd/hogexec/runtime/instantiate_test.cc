#include <gtest/gtest.h>

#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"

namespace hogd {

TEST(WasmRuntimeTest, MissingImportFail)
{
  auto c = test::load_wasm_from_file("test_invoke.wasm");
  WasmiContext context;
  auto runtime = context.make_new_runtime(c, nullptr, true);

  EXPECT_FALSE(!!runtime);
}

} // namespace hogd
