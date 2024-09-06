#include <gtest/gtest.h>

#include "hogd/runtime/host_fn_status.h"
#include "hogd/runtime/wasmi_runtime.h"

#include "hogd/runtime/test_load_wasm.h"

#include <cstdint>

namespace hogd {

TEST(WasmRuntimeTest, MissingImportFail)
{
  auto c = test::load_wasm_from_file("test_invoke.wasm");
  WasmiRuntime runtime(c, nullptr);
  auto res = runtime.invoke("calltest", UINT64_MAX);

  EXPECT_FALSE(!!res.result);

  EXPECT_EQ(res.result.error(), HostFnError{HostFnErrorCode::kWasmiLinkError});
}

} // namespace hogd
