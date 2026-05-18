#include "hogd/hogexec/interface/rpc_from_view_gateway.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "auth_token.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "view_gateway.hh"

namespace hogd {
namespace {

ExpectedViewResult
make_raw_ok(InvocationFrameStatusCode status)
{
  ExpectedViewResult raw(TransactionViewStatus::Ok);
  raw.result().output = InvocationFrameOutput(status);
  return raw;
}

TransactionViewQuery
make_query()
{
  AuthClaim claim(AuthClaimType::kPubkeySignature);
  return TransactionViewQuery{
      .invocation = {},
      .parent_address = {},
      .auth_claim = std::move(claim),
      .forwarded_tokens = {},
      .invoke_flags = {},
  };
}

ViewGatewayResult
make_gateway_ok(BlockHeight height)
{
  ViewGatewayResult result(ViewGatewayOutcome::ViewGatewayOk);
  result.payload().height = height;
  result.payload().output =
      InvocationFrameOutput(InvocationFrameStatusCode::Return);
  result.payload().events = {};
  return result;
}

TEST(ViewGatewayRpc, MapsRawOkOutputs)
{
  for (InvocationFrameStatusCode status :
       {InvocationFrameStatusCode::Return, InvocationFrameStatusCode::Abort,
        InvocationFrameStatusCode::Fault}) {
    ViewGatewayResult result =
        view_gateway_result_from_raw_view_result(7, make_raw_ok(status));

    ASSERT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayOk);
    EXPECT_EQ(result.payload().height, 7);
    EXPECT_EQ(result.payload().output.status_code(), status);
  }
}

TEST(ViewGatewayRpc, MapsInvalidInputToFault)
{
  ExpectedViewResult raw(TransactionViewStatus::InvalidInput);
  raw.error_code() = SyscallErrorCode::kSyscallErrorCodeReentrance;

  ViewGatewayResult result =
      view_gateway_result_from_raw_view_result(9, std::move(raw));

  ASSERT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(result.payload().height, 9);
  EXPECT_EQ(result.payload().output.status_code(),
            InvocationFrameStatusCode::Fault);
  EXPECT_TRUE(result.payload().events.empty());
}

TEST(ViewGatewayRpc, MapsBackendFailures)
{
  EXPECT_EQ(view_gateway_result_from_raw_view_result(
                1, ExpectedViewResult(TransactionViewStatus::Abandoned))
                .outcome(),
            ViewGatewayOutcome::ViewGatewayAbandoned);
  EXPECT_EQ(view_gateway_result_from_raw_view_result(
                1, ExpectedViewResult(TransactionViewStatus::InternalError))
                .outcome(),
            ViewGatewayOutcome::ViewGatewayInternalError);
  EXPECT_EQ(
      view_gateway_result_from_raw_view_result(
          1, ExpectedViewResult(
                 TransactionViewStatus::NondeterministicDataValidationFailure))
          .outcome(),
      ViewGatewayOutcome::ViewGatewayInternalError);
}

TEST(ViewGatewayRpc, NormalizesPublicViewSubset)
{
  ContractAddress caller{};
  TransactionViewQuery query = make_query();
  query.parent_address = caller;
  query.forwarded_tokens.push_back(AuthToken{});
  query.invoke_flags = std::to_underlying(InvokeFlag::TryInvoke) |
                       std::to_underlying(InvokeFlag::DelegateCall);

  TransactionViewQuery normalized =
      normalize_public_view_query(std::move(query));

  ASSERT_TRUE(normalized.parent_address);
  EXPECT_EQ(*normalized.parent_address, caller);
  EXPECT_EQ(normalized.forwarded_tokens.size(), 1U);
  EXPECT_EQ(normalized.invoke_flags, std::to_underlying(InvokeFlag::None));
  ASSERT_EQ(normalized.auth_claim.claim_type(),
            AuthClaimType::kSourceAddressAuthData);
  EXPECT_EQ(normalized.auth_claim.source_address_claim().allowed_keys, 0);
  EXPECT_EQ(normalized.auth_claim.source_address_claim().required_weight, 0);
}

// View queries are read-only simulations.  Public callers are intentionally
// allowed to set parent_address (which the invoked contract observes via
// GetMsgSender) so that off-chain tooling can simulate calls from any context.
// This is safe because view execution never commits state.
TEST(ViewGatewayRpc, NormalizationIntentionallyPreservesParentAddress)
{
  ContractAddress arbitrary_parent{};
  arbitrary_parent[0] = 0xAB;

  TransactionViewQuery query = make_query();
  query.parent_address = arbitrary_parent;

  TransactionViewQuery normalized =
      normalize_public_view_query(std::move(query));

  ASSERT_TRUE(normalized.parent_address)
      << "parent_address must survive normalization (WAI: read-only simulation "
         "context)";
  EXPECT_EQ(*normalized.parent_address, arbitrary_parent);
}

TEST(ViewGatewayRpc, NormalizationIntentionallyPreservesForwardedTokens)
{
  TransactionViewQuery query = make_query();
  query.parent_address = ContractAddress{};
  query.forwarded_tokens.push_back(AuthToken{});
  query.forwarded_tokens.push_back(AuthToken{});

  TransactionViewQuery normalized =
      normalize_public_view_query(std::move(query));

  EXPECT_EQ(normalized.forwarded_tokens.size(), 2U)
      << "forwarded_tokens must survive normalization (WAI: read-only "
         "simulation context)";
}

TEST(ViewGatewayRpc, RetriesAbandonedThenReturnsSuccessfulAttemptHeight)
{
  std::atomic<bool> shutdown_flag = false;
  std::vector<DbTip> tips{
      DbTip{.height = 10, .id = 100},
      DbTip{.height = 11, .id = 101},
  };
  std::uint32_t tip_reads = 0;
  std::uint32_t attempts = 0;
  TransactionViewQuery query = make_query();

  ViewGatewayResult result = run_view_with_retries_for_gateway(
      query, shutdown_flag, [&] { return tips.at(tip_reads++); },
      [&](TransactionViewQuery const &attempt_query, DbTip const &tip) {
        EXPECT_EQ(&attempt_query, &query);
        ++attempts;
        if (attempts == 1) {
          return ViewGatewayResult(ViewGatewayOutcome::ViewGatewayAbandoned);
        }
        return make_gateway_ok(tip.height);
      },
      3);

  ASSERT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(result.payload().height, 11);
  EXPECT_EQ(tip_reads, 2);
  EXPECT_EQ(attempts, 2);
}

TEST(ViewGatewayRpc, ReturnsAbandonedAfterRetryExhaustion)
{
  std::atomic<bool> shutdown_flag = false;
  std::uint32_t attempts = 0;
  TransactionViewQuery query = make_query();

  ViewGatewayResult result = run_view_with_retries_for_gateway(
      query, shutdown_flag, [] { return DbTip{.height = 1, .id = 1}; },
      [&](TransactionViewQuery const &, DbTip const &) {
        ++attempts;
        return ViewGatewayResult(ViewGatewayOutcome::ViewGatewayAbandoned);
      },
      3);

  EXPECT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayAbandoned);
  EXPECT_EQ(attempts, 3);
}

TEST(ViewGatewayRpc, ShutdownShortCircuitsBeforeAttempt)
{
  std::atomic<bool> shutdown_flag = true;
  TransactionViewQuery query = make_query();

  ViewGatewayResult result = run_view_with_retries_for_gateway(
      query, shutdown_flag, [] { return DbTip{.height = 1, .id = 1}; },
      [](TransactionViewQuery const &, DbTip const &) {
        ADD_FAILURE() << "attempt should not run after shutdown";
        return make_gateway_ok(1);
      },
      3);

  EXPECT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayInternalError);
}

TEST(ViewGatewayRpc, ShutdownShortCircuitsBetweenAttempts)
{
  std::atomic<bool> shutdown_flag = false;
  std::uint32_t attempts = 0;
  TransactionViewQuery query = make_query();

  ViewGatewayResult result = run_view_with_retries_for_gateway(
      query, shutdown_flag, [] { return DbTip{.height = 1, .id = 1}; },
      [&](TransactionViewQuery const &, DbTip const &) {
        ++attempts;
        shutdown_flag = true;
        return ViewGatewayResult(ViewGatewayOutcome::ViewGatewayAbandoned);
      },
      3);

  EXPECT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayInternalError);
  EXPECT_EQ(attempts, 1);
}

TEST(ViewGatewayRpc, NotReadyTipDoesNotRunAttempt)
{
  std::atomic<bool> shutdown_flag = false;
  TransactionViewQuery query = make_query();

  ViewGatewayResult result = run_view_with_retries_for_gateway(
      query, shutdown_flag, [] { return DbTip{.height = -1, .id = 0}; },
      [](TransactionViewQuery const &, DbTip const &) {
        ADD_FAILURE() << "attempt should not run without a committed tip";
        return make_gateway_ok(1);
      },
      3);

  EXPECT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayInternalError);
}

} // namespace
} // namespace hogd
