#include "hogd/hog_exec/interface/rpc_from_view_gateway.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "auth_claim.hh"
#include "auth_token.hh"
#include "built_contracts/view_gateway_test.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "view_gateway.hh"
#include "x23/cleanup.h"

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

class ViewGatewayRefreshTest : public ::testing::Test {
public:
  using vgt_meta = ContractTraitMeta<view_gateway_test::ViewGatewayTest>;

  static constexpr ContractAddress kAddr =
      NativeContractTestFramework::make_contract_address(20);

  void SetUp() override
  {
    auto contract = vgt_meta::load_rv32();
    framework_.write_contract(contract);
    framework_.write_deployment(kAddr, contract);
    framework_.setup_grant_storage_token(kAddr, 1'000'000'000);
  }

  static constexpr StorageKeyContractData kKey =
      NativeContractTestFramework::make_storage_key("test_key");

  void commit_bytestring(std::string_view value)
  {
    auto addrkey =
        NativeContractTestFramework::make_contract_data_addrkey(kAddr,
                                                                "test_key");
    auto tx = framework_.make_new_per_tx_db_proxy(
        std::numeric_limits<TransactionID>::max());
    tx.add_delta(addrkey,
                 NativeContractTestFramework::make_bytestring_delta(value),
                 std::to_underlying(StorageDeltaFlags::None));
    auto push = tx.try_push_current_subtransaction();
    ASSERT_TRUE(push.has_value());
    ASSERT_TRUE(push.value());
    ASSERT_TRUE(tx.commit_transaction().has_value());
  }

  static TransactionViewQuery make_read_query()
  {
    ContractTraitClient<view_gateway_test::ViewGatewayTest> client(kAddr);
    return TransactionViewQuery{
        .invocation = client.make_invocation<u8"read_value">(UINT64_MAX, kKey),
        .parent_address = {},
        .auth_claim = AuthClaim(AuthClaimType::kSourceAddressAuthData),
        .forwarded_tokens = {},
        .invoke_flags = {},
    };
  }

  NativeContractTestFramework framework_{
      false, BlockExecutionMode::kBlockExecutionModeProposal, TestDB::kHogDb};

  x23::Defer<> force_commit_ = SystemCallHandler::force_commit_or_abort();
};

TEST_F(ViewGatewayRefreshTest, ViewRefreshesAfterCommittedBlock)
{
  commit_bytestring("old-value");
  framework_.advance_block();

  std::string db_addr =
      std::format("localhost:{}", framework_.get_db_exec_port());
  ViewGatewayRpc gateway(db_addr, "localhost", framework_.get_event_exec_port(),
                         1);

  // First view warms the gateway's ViewState cache.
  ViewGatewayResult r1 = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(r1.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(r1.payload().output.status_code(),
            InvocationFrameStatusCode::Return);
  auto h1 = r1.payload().height;

  // Update the storage value and advance.
  commit_bytestring("new-value");
  framework_.advance_block();

  // The refresh worker should notice the committed tip and refresh the next
  // snapshot without every view polling HogDb.
  ViewGatewayResult r2 =
      ViewGatewayResult(ViewGatewayOutcome::ViewGatewayInternalError);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    r2 = gateway.viewgateway_view(make_read_query())();
    ASSERT_EQ(r2.outcome(), ViewGatewayOutcome::ViewGatewayOk);
    EXPECT_EQ(r2.payload().output.status_code(),
              InvocationFrameStatusCode::Return);
    if (r2.payload().height > h1) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(r2.payload().height, h1);
  auto h2 = r2.payload().height;

  // Explicit refresh remains harmless when there is no newer block.
  gateway.request_refresh();
  ViewGatewayResult r3 = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(r3.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(r3.payload().output.status_code(),
            InvocationFrameStatusCode::Return);
  EXPECT_EQ(r3.payload().height, h2);

  gateway.shut_down();
}

TEST_F(ViewGatewayRefreshTest, RefreshWithNoNewBlockIsHarmless)
{
  commit_bytestring("value");
  framework_.advance_block();

  std::string db_addr =
      std::format("localhost:{}", framework_.get_db_exec_port());
  ViewGatewayRpc gateway(db_addr, "localhost", framework_.get_event_exec_port(),
                         1);

  ViewGatewayResult r1 = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(r1.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  auto h1 = r1.payload().height;

  // Refresh without advancing the block — the tip hasn't changed.
  gateway.request_refresh();
  ViewGatewayResult r2 = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(r2.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(r2.payload().height, h1);

  // Do it again to confirm repeated no-op refreshes are safe.
  gateway.request_refresh();
  gateway.request_refresh();
  ViewGatewayResult r3 = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(r3.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(r3.payload().height, h1);

  gateway.shut_down();
}

} // namespace
} // namespace hogd
