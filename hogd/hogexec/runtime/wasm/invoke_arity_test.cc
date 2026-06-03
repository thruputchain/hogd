// Fairly uninteresting as test cases go but this behavior is critical to
// exactly maintain, and very easy to mess up with a copy-paste bug.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
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

TrampolineResult
expected(int32_t result)
{
  return to_trampoline_result({SyscallErrorCode(result)}, 0);
}

TrampolineResult
unexpected()
{
  return to_trampoline_result(HostFnError{HostFnErrorCode::Unrecoverable}, 0);
}

extern "C" TrampolineResult
arity0(ExternHostCallContext /*unused*/, WasmContext /*unused*/)
{
  return expected(100);
}

extern "C" TrampolineResult
arity1(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0)
{
  if (arg0 != 1) {
    return unexpected();
  }
  return expected(101);
}

extern "C" TrampolineResult
arity2(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1)
{
  if (arg0 != 1 || arg1 != 2) {
    return unexpected();
  }
  return expected(102);
}

extern "C" TrampolineResult
arity3(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1, uint32_t arg2)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3) {
    return unexpected();
  }
  return expected(103);
}

extern "C" TrampolineResult
arity4(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4) {
    return unexpected();
  }
  return expected(104);
}

extern "C" TrampolineResult
arity5(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5) {
    return unexpected();
  }
  return expected(105);
}

extern "C" TrampolineResult
arity6(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
       uint32_t arg5)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5 ||
      arg5 != 6) {
    return unexpected();
  }
  return expected(106);
}

extern "C" TrampolineResult
arity7(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
       uint32_t arg5, uint32_t arg6)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5 ||
      arg5 != 6 || arg6 != 7) {
    return unexpected();
  }
  return expected(107);
}

extern "C" TrampolineResult
arity8(ExternHostCallContext /*unused*/, WasmContext /*unused*/, uint32_t arg0,
       uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4,
       uint32_t arg5, uint32_t arg6, uint32_t arg7)
{
  if (arg0 != 1 || arg1 != 2 || arg2 != 3 || arg3 != 4 || arg4 != 5 ||
      arg5 != 6 || arg6 != 7 || arg7 != 8) {
    return unexpected();
  }
  return expected(108);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class InvokeArityTests : public ::testing::TestWithParam<WasmEngine>,
                         public WasmTestFramework {

protected:
  void SetUp() override
  {
    setup_wasm_context(GetParam());
    Contract contract(ContractType::kWasmi);
    contract.wasmi_contract() =
        test::load_wasm_from_file("test_invoke_arity.wasm");
    auto deployed_contract_hash = DeployedContractHash{
        .hash = sha256(contract),
        .contract_size =
            static_cast<uint32_t>(contract.wasmi_contract().data.size()),
        .contract_type = ContractType::kWasmi,
    };

    link_fn("test", "arg0", &arity0);
    link_fn("test", "arg1", &arity1);
    link_fn("test", "arg2", &arity2);
    link_fn("test", "arg3", &arity3);
    link_fn("test", "arg4", &arity4);
    link_fn("test", "arg5", &arity5);
    link_fn("test", "arg6", &arity6);
    link_fn("test", "arg7", &arity7);
    link_fn("test", "arg8", &arity8);
    make_syscall_handler(contract);

    auto runtime_res = get_context()->make_new_runtime(deployed_contract_hash,
                                                       get_syscall_handler());
    ASSERT_TRUE(runtime_res.has_value());
    runtime_ = std::move(runtime_res.value());

    ASSERT_TRUE(runtime_);
  }

  std::unique_ptr<Runtime> runtime_;
};

TEST_P(InvokeArityTests, arity0)
{
  auto res = runtime_->invoke("calltest0", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity1)
{
  auto res = runtime_->invoke("calltest1", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity2)
{
  auto res = runtime_->invoke("calltest2", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity3)
{
  auto res = runtime_->invoke("calltest3", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity4)
{
  auto res = runtime_->invoke("calltest4", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity5)
{
  auto res = runtime_->invoke("calltest5", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity6)
{
  auto res = runtime_->invoke("calltest6", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity7)
{
  auto res = runtime_->invoke("calltest7", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

TEST_P(InvokeArityTests, arity8)
{
  auto res = runtime_->invoke("calltest8", UINT64_MAX);
  EXPECT_TRUE(res.result_);
}

INSTANTIATE_TEST_SUITE_P(WasmEngines, InvokeArityTests,
                         ::testing::Values(WasmEngine::kWasmi,
                                           WasmEngine::kWasmtime),
                         [](const ::testing::TestParamInfo<WasmEngine> &info) {
                           return WasmTestFramework::engine_to_string(
                               info.param);
                         });

} // namespace hogd
