#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "syscall.hh"

namespace hogd {

namespace {

SystemCallHandler *ctx_check =
    reinterpret_cast<SystemCallHandler *>(0xAABBCCDD'EEFF0011);

HostFnStatus<SyscallErrorCode>
foo(HostCallContext *ctx, uint64_t value)
{
  EXPECT_TRUE(ctx->syscall_handler == ctx_check);
  EXPECT_TRUE(value == 12);
  return SyscallErrorCode(15);
}

} // namespace

class UserContextTest : public ::testing::TestWithParam<RuntimeEngine> {};

TEST_P(UserContextTest, CheckCtxCorrect)
{
  Contract contract(ContractType::kWasmi);
  contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");
  auto context = make_runtime_context(GetParam());

  context->link_fn("test", "redir_call", &foo);

  auto runtime = context->make_new_runtime(contract, ctx_check, true);

  ASSERT_TRUE(runtime);

  auto res = runtime->invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

INSTANTIATE_TEST_SUITE_P(AllEngines, UserContextTest,
                         ::testing::Values(RuntimeEngine::kWasmi,
                                           RuntimeEngine::kWasm3),
                         [](const testing::TestParamInfo<RuntimeEngine> &info) {
                           return engine_to_string(info.param);
                         });

} // namespace hogd
