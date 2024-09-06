#include <gtest/gtest.h>

#include "hogd/runtime/host_fn_status.h"
#include "hogd/runtime/test_load_wasm.h"
#include "hogd/runtime/wasmi_runtime.h"
#include "hogd/base/types.h"
#include <cstdint>

namespace hogd {

void *ctx_check = reinterpret_cast<void *>(0xAABBCCDD'EEFF0011);

HostFnStatus<uint64_t>
foo(HostCallContext *ctx, uint64_t value)
{
  EXPECT_TRUE(ctx->user_ctx == ctx_check);
  EXPECT_TRUE(value == 12);
  return 15;
}

TEST(UserCtxTest, CheckCtxCorrect)
{
  auto c = test::load_wasm_from_file("test_invoke.wasm");

  WasmiRuntime runtime(c, ctx_check);

  runtime.template link_fn<&foo>("test", "redir_call");

  auto res = runtime.invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(!!res.result);
  EXPECT_EQ(*res.result, 15U);
}

} // namespace hogd
