// Regression test for issue #2682: a test_ffi runtime-context handle must stay
// usable after the thread that opened it exits.
//
// open_runtime_context() hands the caller a heap handle wrapping the
// MultiplexedContext built by make_hog_exec_context(). That multiplexer borrows
// its native and RV32 sub-contexts as non-owning raw pointers, and those
// sub-contexts used to live in `thread_local` storage. The handle's documented
// lifetime is governed by close_runtime_context(), not by the creating thread,
// so an honest C-API caller could open a handle on a short-lived thread, join
// it, and then dispatch through raw pointers that the creator thread's TLS
// destructors had already freed -- an ASan heap-use-after-free inside
// MultiplexedContext::verify_contract() / make_new_runtime().
//
// The sub-contexts are now shared process-lifetime statics (see
// hogd/hog_exec/precompiles/context.cc), so the borrow can no longer dangle.
// Both dispatch entry points are const, which is what keeps them safe to share.
//
// This test asserts the honest-caller expectation the issue's PoCs violated:
// verification through the transferred handle succeeds normally. Under ASan it
// additionally pins the absence of the use-after-free.

#include <thread>

#include <gtest/gtest.h>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "precompiles.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

TEST(TestBindingsTest, VerifyNativeContractAfterCreatorThreadExit)
{
  OpenRuntimeContextResult opened{.context = nullptr,
                                  .ffi_error_code =
                                      kTestFFIErrorCodeUnrecoverableError};

  // Open on a thread and join it, so the creator's thread_local destructors
  // have run by the time the handle is used below.
  std::thread creator([&opened] { opened = open_runtime_context(); });
  creator.join();

  ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(opened.context, nullptr);
  x23::Defer close_rc([&opened] {
    EXPECT_EQ(close_runtime_context(opened.context), kTestFFIErrorCodeNone);
  });

  Contract contract(ContractType::kNative);
  contract.native_contract_id() =
      HogexecPrecompiledNativeContracts::kNativeContractHogToken;
  auto bytes = x23::serialize(contract);

  // Dispatches through MultiplexedContext -> the borrowed native sub-context.
  auto result = do_verify_contract(opened.context, bytes.data(),
                                   static_cast<uint32_t>(bytes.size()));
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(result.success, 1);
}

// The same escaped-lifetime question for the RV32 arm of the multiplexer, which
// borrows its sub-context the same way. A bare kRv32 Contract need not verify
// successfully -- what matters is that the dispatch itself is well-defined and
// returns a structured result rather than reading freed storage.
TEST(TestBindingsTest, VerifyRv32ContractAfterCreatorThreadExitIsDefined)
{
  OpenRuntimeContextResult opened{.context = nullptr,
                                  .ffi_error_code =
                                      kTestFFIErrorCodeUnrecoverableError};

  std::thread creator([&opened] { opened = open_runtime_context(); });
  creator.join();

  ASSERT_EQ(opened.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(opened.context, nullptr);
  x23::Defer close_rc([&opened] {
    EXPECT_EQ(close_runtime_context(opened.context), kTestFFIErrorCodeNone);
  });

  Contract contract(ContractType::kRv32);
  auto bytes = x23::serialize(contract);

  auto result = do_verify_contract(opened.context, bytes.data(),
                                   static_cast<uint32_t>(bytes.size()));
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
}

} // namespace hogd
