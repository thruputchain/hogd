// kRistretto255Msm on empty input, driven through the public extern "C" API.
//
// This test used to live in mock_syscall_test.cc, where it built a DBContext,
// SyscallHandlerContext and EmptyRuntime directly and then made its first FFI
// call. That call ran lazy_init(), which replaces the process logger while
// the already-open DB's background thread may be logging: a TSAN data race on
// hogd::detail::logger (thruputchain/benchmarks#45). Going through the C API
// end to end, as a real consumer does, runs lazy_init() before the DB exists.

#include <array>
#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/components/contract_trait.h"
#include "hogd/hog_exec/runtime/gas_cost_table.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/tests/test_ffi_test_helpers.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

TEST(TestBindingsTest, Ristretto255MsmEmptySum)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_defer([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_defer([rc] { close_runtime_context(rc); });

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);

  ASSERT_EQ(open_frame(handler.syscall_handler,
                       Invocation{
                           .invoked_address = ContractAddress{0xA0},
                           .entrypoint = entrypoint_from_method_id(0x1111),
                           .gas_limit = 1'000'000,
                           .calldata = {},
                       }),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  std::array<uint8_t, 32> output;
  output.fill(0xA5);
  // Empty input accepts a null pointer and returns the canonical identity.
  auto res =
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kRistretto255Msm),
                              0, 0, reinterpret_cast<uint64_t>(output.data()),
                              0, 0, 0, 0);
  EXPECT_EQ(res.panic, std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(res.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
  EXPECT_EQ(output, (std::array<uint8_t, 32>{}));

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);

  auto finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  x23::Defer free_finish([&] {
    free_ffi_data_buffer(finish.serialized_output);
    free_ffi_data_buffer(finish.serialized_trace);
    free_ffi_data_buffer(finish.serialized_events);
  });
  ASSERT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  EXPECT_EQ(finish.gas_consumed,
            kSyscallBaseCost + GasCost::ristretto255_msm(0));

  EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
}

} // namespace
} // namespace hogd
