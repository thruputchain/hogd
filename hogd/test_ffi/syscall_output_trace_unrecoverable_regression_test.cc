// Regression test for the "Traced <syscall> Output Allocation Panics" cluster:
// issues #2697 (GetCurrentAddress), #2745 (GetBlockHeight),
// #2746 (GetTransactionStatistics), #2750 (RetrocausalQueuePeekLen),
// #2753 (GetDeployAddress), and #2754 (SHA256). See test_ffi_test_helpers.h.
//
// Each of these syscalls computes its real result and writes the caller's
// output, then materializes an optional XDR trace-output record
// (`trace_data->outputs.emplace(...)`, which allocates with `new`). The catch
// block around that emplace used to call panic() on allocation failure, which
// terminates the process -- crossing the noexcept syscall-body boundary in
// wrap_after_parse() before mock_syscall_entrypoint() could translate it into a
// structured ForeignHostFnResult. The catches now report the failure through
// the no-allocation reporter and return HostFnErrorCode::Unrecoverable instead,
// so an allocation failure aborts the invocation with a structured error rather
// than killing the process.
//
// These tests run the trace-output recording path under trace-level logging
// with tracing enabled and assert each syscall still succeeds on the normal
// path, writes its real output, and produces a serialized trace. The
// allocation-failure branch itself can only be forced with a global allocator
// hook, which (matching trace_syscall_logging_2561_2562_regression_test.cc) we
// deliberately do not install here. The fix is identical across every
// syscall-specific trace-output catch; these cover a representative span of the
// affected files.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {
namespace {

// Run `body` against a traced mock invocation frame (empty calldata), then pop
// the context and finish the frame, asserting that tracing produced a
// serialized trace.
template<typename Body>
void
run_in_traced_frame(Body &&body, ContractAddress contract_addr = {0x42})
{
  enable_syscall_logging(kTestFFILogLevelTrace);
  x23::Defer reset_logging(
      [] { enable_syscall_logging(kTestFFILogLevelError); });

  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  // Some syscalls (e.g. RetrocausalQueue*) write storage and need the frame's
  // contract to hold storage tokens. Granting them is harmless for the rest.
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, contract_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);
  x23::Defer d3([&] { close_syscall_handler(handler.syscall_handler); });

  Invocation invocation = {
      .invoked_address = contract_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  body(handler.syscall_handler);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish_res =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish_res.ffi_error_code, kTestFFIErrorCodeNone);
  // Tracing was enabled, so the finished frame carries a serialized trace built
  // by the trace-output recording paths.
  EXPECT_NE(finish_res.serialized_trace.data, nullptr);
  EXPECT_GT(finish_res.serialized_trace.len, 0U);
  free_ffi_data_buffer(finish_res.serialized_output);
  free_ffi_data_buffer(finish_res.serialized_trace);
  free_ffi_data_buffer(finish_res.serialized_events);
}

void
expect_syscall_ok(ForeignHostFnResult const &result)
{
  EXPECT_EQ(result.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(result.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
}

} // namespace

// #2745: GetBlockHeight writes the height, then records its trace output.
TEST(TestBindingsTest, TracedGetBlockHeightOutputRecordsTraceWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    BlockHeight height = -1;
    auto result =
        mock_syscall_entrypoint(std::to_underlying(SyscallNo::kGetBlockHeight),
                                reinterpret_cast<uint64_t>(&height), 0, 0, 0, 0,
                                0, 0);
    expect_syscall_ok(result);
    EXPECT_EQ(height, 0);
  });
}

// #2754: SHA256 writes the digest, then records its trace output.
TEST(TestBindingsTest, TracedSha256OutputRecordsTraceWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    std::array<std::byte, 3> input{std::byte{0x61}, std::byte{0x62},
                                   std::byte{0x63}};
    SHA256Hash output_hash{};
    auto result =
        mock_syscall_entrypoint(std::to_underlying(SyscallNo::kSHA256),
                                reinterpret_cast<uint64_t>(input.data()),
                                input.size(),
                                reinterpret_cast<uint64_t>(&output_hash), 0, 0,
                                0, 0);
    expect_syscall_ok(result);
    EXPECT_EQ(output_hash, sha256(std::span<const std::byte>(input)));
  });
}

// #2697: GetCurrentAddress writes the frame address, then records its trace
// output.
TEST(TestBindingsTest, TracedGetCurrentAddressOutputRecordsTraceWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    ContractAddress address{};
    auto result = mock_syscall_entrypoint(std::to_underlying(
                                              SyscallNo::kGetCurrentAddress),
                                          reinterpret_cast<uint64_t>(&address),
                                          0, 0, 0, 0, 0, 0);
    expect_syscall_ok(result);
    EXPECT_EQ(address, ContractAddress{0x42});
  });
}

// #2753: GetDeployAddress computes the address (null caller -> current frame),
// then records its trace output.
TEST(TestBindingsTest, TracedGetDeployAddressOutputRecordsTraceWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    SHA256Hash nonce{};
    SHA256Hash contract_hash{};
    ContractAddress to_be_deployed{};
    auto result = mock_syscall_entrypoint(
        std::to_underlying(SyscallNo::kGetDeployAddress),
        /*optional caller_address=*/0, reinterpret_cast<uint64_t>(&nonce),
        reinterpret_cast<uint64_t>(&contract_hash),
        reinterpret_cast<uint64_t>(&to_be_deployed), 0, 0, 0);
    expect_syscall_ok(result);
  });
}

// #2746: GetTransactionStatistics(GasRemaining) writes the stat, then records
// its trace output.
TEST(TestBindingsTest,
     TracedGetTransactionStatisticsOutputRecordsTraceWithoutCrashing)
{
  run_in_traced_frame([](void * /*handler*/) {
    uint64_t out_stat = 0;
    auto result = mock_syscall_entrypoint(
        std::to_underlying(SyscallNo::kGetTransactionStatistics),
        std::to_underlying(TransactionStatistic::GasRemaining),
        reinterpret_cast<uint64_t>(&out_stat), 0, 0, 0, 0, 0);
    expect_syscall_ok(result);
    EXPECT_GT(out_stat, 0U);
  });
}

// #2750: RetrocausalQueuePeekLen stages the entry length, then records its
// trace output.
TEST(TestBindingsTest,
     TracedRetrocausalQueuePeekLenOutputRecordsTraceWithoutCrashing)
{
  run_in_traced_frame([](void *handler) {
    StorageKeyContractData key{};
    std::array<std::byte, 3> payload{std::byte{0x10}, std::byte{0x20},
                                     std::byte{0x30}};
    auto insert_result = mock_syscall_entrypoint(
        std::to_underlying(SyscallNo::kRetrocausalQueueInsert),
        reinterpret_cast<uint64_t>(&key),
        reinterpret_cast<uint64_t>(payload.data()), payload.size(), 0, 0, 0, 0);
    expect_syscall_ok(insert_result);

    RuntimeMemoryLength output_len = 0;
    auto peek_result = mock_syscall_entrypoint(
        std::to_underlying(SyscallNo::kRetrocausalQueuePeekLen),
        reinterpret_cast<uint64_t>(&key),
        reinterpret_cast<uint64_t>(&output_len), 0, 0, 0, 0, 0);
    expect_syscall_ok(peek_result);
    EXPECT_EQ(output_len, payload.size());
    (void)handler;
  });
}

} // namespace hogd
