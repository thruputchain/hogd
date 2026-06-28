// Regression test for issue #2643: test_ffi root runtime-resolution error then a
// second root_invoke re-enters the consumed root invocation.
//
// SystemCallHandler::root_invoke() moves the root invocation out of
// root_invocation_ before resolving the target runtime. If runtime creation
// throws after that point but before any frame starts, the FFI wrapper returns a
// structured error while leaving the handler live and last_frame_finished_
// false. A second root_invoke() must also be rejected with a structured error
// rather than reaching the core CHECK(root_invocation_.has_value()).

#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/item/deployed_contract.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/test_ffi/mock_runtime.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

SignedTransaction
make_mock_root_transaction(ContractAddress const &deployment_addr)
{
  return SignedTransaction{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height =
                                std::numeric_limits<int64_t>::max(),
                            .nonce = 0}},
      .signatures = {},
  };
}

// A runtime context that throws std::bad_alloc the first time it resolves a
// mock-test runtime, modeling an allocation failure at the MockRuntime
// construction point after contract lookup has already succeeded.
class RuntimeResolutionThrowingMockRuntimeContext final
  : public MockRuntimeContext {
public:
  using MockRuntimeContext::MockRuntimeContext;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override
  {
    if (throw_after_mock_lookup_ &&
        deployed_contract.hash.contract_type == ContractType::kMockTest) {
      auto contract =
          syscall_handler->resolve_contract(deployed_contract.hash.hash);
      if (!contract.has_value()) {
        return contract.error();
      }
      if (contract.value() == nullptr ||
          contract.value()->contract_type() != ContractType::kMockTest) {
        return SystemError{SystemErrorCode::Unrecoverable};
      }
      throw_after_mock_lookup_ = false;
      throw std::bad_alloc();
    }
    return MockRuntimeContext::make_new_runtime(deployed_contract,
                                                syscall_handler);
  }

  void throw_after_mock_lookup_once() { throw_after_mock_lookup_ = true; }

private:
  bool throw_after_mock_lookup_ = false;
};

} // namespace

TEST(TestBindingsTest,
     RuntimeResolutionErrorThenSecondRootInvokeRejectedNotAsserted)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_defer([db] { free_db(db); });

  auto runtime_context =
      std::make_shared<RuntimeResolutionThrowingMockRuntimeContext>(
          make_hogexec_context());
  std::shared_ptr<MockRuntimeContext> runtime_context_handle = runtime_context;
  auto *rc = &runtime_context_handle;

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_return, nullptr);
  auto stx = make_mock_root_transaction(deployment_addr);

  // Control: a non-faulted invocation proves the setup is valid.
  {
    auto *control_handler = make_syscall_handler(db, rc, stx);
    ASSERT_NE(control_handler, nullptr);
    x23::Defer close_control(
        [control_handler] { close_syscall_handler(control_handler); });

    auto control = root_invoke(control_handler, 0);
    EXPECT_EQ(control.ffi_error_code, kTestFFIErrorCodeNone);
    free_ffi_data_buffer(control.serialized_output);
    free_ffi_data_buffer(control.serialized_trace);
    free_ffi_data_buffer(control.serialized_events);
    EXPECT_EQ(abort_transaction(control_handler), kTestFFIErrorCodeNone);
  }

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);
  x23::Defer close_faulted(
      [syscall_handler] { close_syscall_handler(syscall_handler); });

  runtime_context->throw_after_mock_lookup_once();
  auto first = root_invoke(syscall_handler, 0);
  EXPECT_EQ(first.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(first.serialized_output.data, nullptr);

  // The faulted handler is still live; a second root_invoke() must be rejected
  // with a structured error rather than re-entering the consumed root invocation
  // and tripping the core CHECK.
  auto second = root_invoke(syscall_handler, 0);
  EXPECT_EQ(second.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(second.serialized_output.data, nullptr);
}

} // namespace hogd
