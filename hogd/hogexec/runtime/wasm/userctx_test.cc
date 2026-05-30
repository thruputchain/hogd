#include <cstdint>

#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/runtime_exports.h"
#include "hogd/hogexec/runtime/trampoline_result.h"
#include "hogd/hogexec/runtime/wasm/test_load_wasm.h"
#include "hogd/hogexec/runtime/wasm/wasm_test_framework.h"
#include "syscall_error.hh"

namespace hogd {

namespace {

SystemCallHandler *ctx_check = nullptr;

extern "C" TrampolineResult
foo(ExternHostCallContext external_context, WasmContext /*unused*/,
    uint32_t value)
{
  auto *ctx = reinterpret_cast<HostCallContext *>(external_context);
  EXPECT_TRUE(ctx->syscall_handler == ctx_check);
  EXPECT_TRUE(value == 12);
  return to_trampoline_result({SyscallErrorCode(15)}, 0);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class UserCtxTest : public ::testing::TestWithParam<WasmEngine>,
                    public WasmTestFramework {
protected:
  void SetUp() override
  {
    Contract contract(ContractType::kWasmi);
    contract.wasmi_contract() = test::load_wasm_from_file("test_invoke.wasm");

    deployed_contract_hash_ = DeployedContractHash{
        .hash = sha256(contract),
        .contract_size =
            static_cast<uint32_t>(contract.wasmi_contract().wasm.size()),
        .contract_type = ContractType::kWasmi,
    };

    setup_wasm_context(GetParam());
    link_fn("test", "redir_call", &foo);
    make_syscall_handler(contract);
  }

  DeployedContractHash deployed_contract_hash_;
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST_P(UserCtxTest, CheckCtxCorrect)
{
  auto runtime = get_context()->make_new_runtime(deployed_contract_hash_,
                                                 get_syscall_handler());
  ASSERT_TRUE(runtime.has_value());
  ASSERT_TRUE(runtime.value() != nullptr);

  ctx_check = get_syscall_handler();

  auto res = runtime.value()->invoke("calltest", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}
// NOLINTEND(bugprone-unchecked-optional-access)

INSTANTIATE_TEST_SUITE_P(WasmEngines, UserCtxTest,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
