// Regression test for the block timepoint the test_ffi chain reports.
// See test_ffi_test_helpers.h.

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

// What GetBlockTimepoint reported inside the contract, one entry per
// invocation, recorded for the assertions after the invocations return.
std::vector<TimePoint> g_observed_timepoints;

ForeignInvokeResult
mock_contract_read_timepoint(void * /*unused*/, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  g_observed_timepoints.push_back(context->to_handler().get_block_timepoint());

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

// Runs one transaction against `db` whose only effect is to record what
// GetBlockTimepoint reports.
void
invoke_reading_timepoint(void *db, void *rc, ContractAddress const &addr)
{
  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);

  auto result = root_invoke(syscall_handler, 0);
  ASSERT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_events);
  free_ffi_data_buffer(result.serialized_trace);

  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

TimePoint
now_nanos()
{
  return static_cast<TimePoint>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

// DBContext reported a hardcoded block_timepoint of 1'700'000'000 for every
// block. TimePoint is Unix nanoseconds, so that is 1.7 seconds after the epoch
// -- a Unix *seconds* value in a nanoseconds field -- and it never moved, so no
// contract keyed on time (a deadline, an expiry, a vesting schedule, a rate
// limit) could be exercised at all.
//
// Advancing with no timepoint now takes the wall clock, which is what a caller
// with no opinion wants: a plausible present-day value that moves between
// blocks.
TEST(TestBindingsTest, AdvanceWithoutATimepointTakesTheWallClock)
{
  g_observed_timepoints.clear();

  const TimePoint before = now_nanos();

  void *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_read_timepoint, nullptr);

  invoke_reading_timepoint(db, rc, deployment_addr);
  ASSERT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
  invoke_reading_timepoint(db, rc, deployment_addr);

  const TimePoint after = now_nanos();

  ASSERT_EQ(g_observed_timepoints.size(), 2u);
  for (TimePoint observed : g_observed_timepoints) {
    // Bracketed by wall-clock reads taken around the whole test, which is what
    // distinguishes a real nanosecond timestamp from the old constant: 1.7e9
    // is below `before` by more than fifty years.
    EXPECT_GE(observed, before);
    EXPECT_LE(observed, after);
  }
  // Two blocks, two clock reads: time moved.
  EXPECT_GT(g_observed_timepoints[1], g_observed_timepoints[0]);
}

// A test driving contract logic keyed on time needs the timepoint pinned, not
// merely plausible, so advance_new_block() takes one.
TEST(TestBindingsTest, AdvanceWithATimepointPinsIt)
{
  g_observed_timepoints.clear();

  // 2026-01-01T00:00:00Z in Unix nanoseconds, far enough from any wall clock
  // this runs under that a host ignoring the argument fails.
  const uint64_t pinned = 1'767'225'600'000'000'000ULL;

  void *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_read_timepoint, nullptr);

  ASSERT_EQ(advance_new_block(db, &pinned), kTestFFIErrorCodeNone);
  invoke_reading_timepoint(db, rc, deployment_addr);

  // Fixed for the block: a second read in the same block agrees.
  invoke_reading_timepoint(db, rc, deployment_addr);

  ASSERT_EQ(g_observed_timepoints.size(), 2u);
  EXPECT_EQ(g_observed_timepoints[0], pinned);
  EXPECT_EQ(g_observed_timepoints[1], pinned);
}

// The commit path builds its ConsensusValue from scratch, so a field it does
// not set is committed as zero however the block actually executed. That is
// how the network hash went missing (#3756); the timepoint sat in the same
// gap, leaving the committed header claiming 0 for a block whose contracts had
// run against a real value.
//
// Read back through the next block's snapshot cache, whose db tip *is* the
// committed parent.
TEST(TestBindingsTest, AdvanceCommitsTheTimepointItRanWith)
{
  const uint64_t pinned = 1'767'225'600'000'000'000ULL;

  void *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  // Open a block on `pinned`, then advance again so that block is committed.
  ASSERT_EQ(advance_new_block(db, &pinned), kTestFFIErrorCodeNone);
  ASSERT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);

  auto *typed_db = reinterpret_cast<DBContext *>(db);
  auto tip = typed_db->get_snapshot().try_get_db_tip();
  ASSERT_TRUE(tip.has_value());
  ASSERT_NE(*tip, nullptr);
  EXPECT_EQ((*tip)->value.proposal_header.block_timepoint, pinned);
}

} // namespace hogd
