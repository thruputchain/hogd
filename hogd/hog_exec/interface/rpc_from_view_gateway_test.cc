#include "hogd/hog_exec/interface/rpc_from_view_gateway.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "auth_token.hh"
#include "built_contracts/authclaim_test.h"
#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
#include "built_contracts/manual_revert_test.h"
#include "built_contracts/view_gateway_test.h"
#include "executor_to_db_rpc.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/version.h"
#include "hogd/xdr/protocol_token.h"
#include "invoke_flag.hh"
#include "network_config.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "view_gateway.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"

namespace hogd {
namespace {

ExpectedViewResult
make_raw_ok(InvocationFrameStatusCode status)
{
  ExpectedViewResult raw(TransactionViewStatus::Ok);
  raw.result().output = InvocationFrameOutput(status);
  raw.result().gas_consumed = 1234;
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
    EXPECT_EQ(result.payload().gas_consumed, 1234U);
  }
}

TEST(ViewGatewayRpc, MapsInvalidInputToInvalidInput)
{
  ExpectedViewResult raw(TransactionViewStatus::InvalidInput);
  raw.error_code() = SyscallErrorCode::kSyscallErrorCodeReentrance;

  ViewGatewayResult result =
      view_gateway_result_from_raw_view_result(9, std::move(raw));

  ASSERT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayInvalidInput);
  EXPECT_EQ(result.error_code(), SyscallErrorCode::kSyscallErrorCodeReentrance);
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
  const auto claim = query.auth_claim;
  query.parent_address = caller;
  query.forwarded_tokens.push_back(x23::serialize(AuthToken{}));
  query.invoke_flags = std::to_underlying(InvokeFlag::TryInvoke) |
                       std::to_underlying(InvokeFlag::DelegateCall);

  TransactionViewQuery normalized =
      normalize_public_view_query(std::move(query));

  ASSERT_TRUE(normalized.parent_address);
  EXPECT_EQ(*normalized.parent_address, caller);
  EXPECT_EQ(normalized.forwarded_tokens.size(), 1U);
  EXPECT_EQ(normalized.invoke_flags, std::to_underlying(InvokeFlag::None));
  EXPECT_EQ(normalized.auth_claim, claim);
}

TEST(ViewGatewayRpc, PreservesSourceAddressClaimAndDefault)
{
  auto query = make_query();
  query.auth_claim = AuthClaim(AuthClaimType::kSourceAddressAuthData);
  EXPECT_EQ(normalize_public_view_query(query).auth_claim, query.auth_claim);
  query.auth_claim.source_address_claim() = {.allowed_keys = 5,
                                             .required_weight = 3};
  EXPECT_EQ(normalize_public_view_query(query).auth_claim, query.auth_claim);
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
  query.forwarded_tokens.push_back(x23::serialize(AuthToken{}));
  query.forwarded_tokens.push_back(x23::serialize(AuthToken{}));

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

  ViewGatewayRefreshTest() : ViewGatewayRefreshTest(stock_network_config()) {}

  // For a chain whose genesis states `network_config`.
  explicit ViewGatewayRefreshTest(NetworkConfig const &network_config)
    : framework_(false, BlockExecutionMode::kBlockExecutionModeProposal,
                 TestDB::kHogDb, true, std::nullopt, network_config)
  {}

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
    auto push = tx.try_push_current_subtransaction()();
    ASSERT_TRUE(push.has_value());
    ASSERT_TRUE(push.value());
    ASSERT_TRUE(tx.commit_transaction()().has_value());
  }

  static TransactionViewQuery make_read_query()
  {
    ContractTraitClient<view_gateway_test::ViewGatewayTest> client(kAddr);
    return TransactionViewQuery{
        .invocation =
            client.make_invocation<u8"read_value">(kDefaultMaxTxGas, kKey),
        .parent_address = {},
        .auth_claim = AuthClaim(AuthClaimType::kSourceAddressAuthData),
        .forwarded_tokens = {},
        .invoke_flags = {},
    };
  }

  NativeContractTestFramework framework_;

  x23::Defer<> force_commit_ = SystemCallHandler::force_commit_or_abort();
};

TEST_F(ViewGatewayRefreshTest, ViewRefreshesAfterCommittedBlock)
{
  commit_bytestring("old-value");
  framework_.advance_block();

  std::string db_addr =
      std::format("localhost:{}", framework_.get_db_exec_port());
  ViewGatewayRpc gateway(db_addr, "localhost",
                         framework_.get_event_exec_port());

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
  ViewGatewayRpc gateway(db_addr, "localhost",
                         framework_.get_event_exec_port());

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

// viewgateway_intro: only the exact protocol commit is accepted; anything
// else is ViewGatewayIntroMismatchedProtocol. Only the success arm was pinned
// (via devnode) before. Also pins the shutdown arm of viewgateway_view: a
// view after shut_down() is ViewGatewayInternalError, not a hang or a stale
// answer.
TEST_F(ViewGatewayRefreshTest, IntroMismatchAndViewAfterShutdown)
{
  commit_bytestring("value");
  framework_.advance_block();

  std::string db_addr =
      std::format("localhost:{}", framework_.get_db_exec_port());
  ViewGatewayRpc gateway(db_addr, "localhost",
                         framework_.get_event_exec_port());

  EXPECT_EQ(gateway.viewgateway_intro(protocol_from_text("not-a-protocol")),
            ViewGatewayIntroOutcome::ViewGatewayIntroMismatchedProtocol);
  EXPECT_EQ(gateway.viewgateway_intro(version::kHogdProtocol()),
            ViewGatewayIntroOutcome::ViewGatewayIntroSuccess);

  ViewGatewayResult before = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(before.outcome(), ViewGatewayOutcome::ViewGatewayOk);

  gateway.shut_down();

  ViewGatewayResult after = gateway.viewgateway_view(make_read_query())();
  EXPECT_EQ(after.outcome(), ViewGatewayOutcome::ViewGatewayInternalError);
}

// The abandon-rotate-retry machine, driven deterministically: a committed
// block invalidates the gateway's viewing snapshot (they are siblings off the
// same parent), but a repeat query of an already-cached key is served from
// the ViewState's snapshot cache and legitimately reports the STALE height --
// stale-but-consistent, never an error. A query that must touch the DB (an
// uncached key) observes the invalidation as an Abandoned attempt,
// run_view_with_retries rotates to a fresh snapshot, and the caller sees Ok
// at the NEW height. Neither path may ever surface ViewGatewayAbandoned.
TEST_F(ViewGatewayRefreshTest, RotationOnStaleSnapshotServesFreshHeight)
{
  commit_bytestring("value-0");
  framework_.advance_block();

  std::string db_addr =
      std::format("localhost:{}", framework_.get_db_exec_port());
  ViewGatewayRpc gateway(db_addr, "localhost",
                         framework_.get_event_exec_port());

  ViewGatewayResult warm = gateway.viewgateway_view(make_read_query())();
  ASSERT_EQ(warm.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  auto height = warm.payload().height;

  for (int i = 1; i <= 4; ++i) {
    commit_bytestring(std::format("value-{}", i));
    framework_.advance_block();

    // Cached key: Ok at whatever height the current snapshot has (stale
    // until something forces a rotation) -- never an error.
    ViewGatewayResult cached = gateway.viewgateway_view(make_read_query())();
    ASSERT_EQ(cached.outcome(), ViewGatewayOutcome::ViewGatewayOk);
    EXPECT_GE(cached.payload().height, height);

    // Uncached key: the DB read sees the sibling-invalidated snapshot,
    // the attempt abandons, and rotation must land on the new tip.
    auto query = make_read_query();
    ContractTraitClient<view_gateway_test::ViewGatewayTest> client(kAddr);
    query.invocation = client.make_invocation<u8"read_value">(
        kDefaultMaxTxGas, NativeContractTestFramework::make_storage_key(
                              std::format("uncached_key_{}", i)));
    ViewGatewayResult fresh = gateway.viewgateway_view(std::move(query))();
    ASSERT_EQ(fresh.outcome(), ViewGatewayOutcome::ViewGatewayOk);
    EXPECT_GT(fresh.payload().height, height);
    height = fresh.payload().height;
  }

  gateway.shut_down();
}

// End-to-end proof that viewgateway_view actually APPLIES
// normalize_public_view_query: a query carrying TryInvoke|DelegateCall
// invoke flags still executes as a plain root view (Return), where the
// un-normalized flags would be rejected by the view runner. The unit tests
// pin what normalization strips; this pins that the RPC path calls it.
TEST_F(ViewGatewayRefreshTest, ViewNormalizesFlagsEndToEnd)
{
  commit_bytestring("value");
  framework_.advance_block();

  std::string db_addr =
      std::format("localhost:{}", framework_.get_db_exec_port());
  ViewGatewayRpc gateway(db_addr, "localhost",
                         framework_.get_event_exec_port());

  auto query = make_read_query();
  query.invoke_flags = std::to_underlying(InvokeFlag::TryInvoke) |
                       std::to_underlying(InvokeFlag::DelegateCall);

  ViewGatewayResult r = gateway.viewgateway_view(std::move(query))();
  ASSERT_EQ(r.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(r.payload().output.status_code(),
            InvocationFrameStatusCode::Return);

  gateway.shut_down();
}

// A chain allowing auth tokens deeper than x23's default decode bound.
class ViewGatewayDeepTokenTest : public ViewGatewayRefreshTest {
public:
  static constexpr uint32_t kChainDepth = 20;
  static constexpr ContractAddress kParentAddr =
      NativeContractTestFramework::make_contract_address(21);

  ViewGatewayDeepTokenTest() : ViewGatewayRefreshTest(deep_token_config()) {}

  static NetworkConfig deep_token_config()
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxAuthTokenDepth,
                                   kChainDepth);
    return config;
  }

  static TransactionViewQuery make_query_forwarding(uint32_t depth)
  {
    AuthTokenNode node{.consumer_address = kAddr,
                       .payload = {},
                       .child_nodes = {}};
    for (uint32_t i = 1; i < depth; ++i) {
      node = AuthTokenNode{.consumer_address = kAddr,
                           .payload = {},
                           .child_nodes = {node}};
    }
    auto query = make_read_query();
    query.parent_address = kParentAddr;
    query.forwarded_tokens.push_back(
        x23::serialize(AuthToken{.mint_address = kParentAddr,
                                 .expiry_height = 1'000'000,
                                 .root_node = std::move(node)}));
    return query;
  }
};

// Over RPC, a view takes a forwarded token exactly as deep as the chain allows,
// past x23's default decode bound, and refuses one level deeper.
TEST_F(ViewGatewayDeepTokenTest, ViewRpcAdmitsTheChainsTokenDepth)
{
  ASSERT_GT(kChainDepth, x23::deserialization_recursion_limit);
  commit_bytestring("value");
  framework_.advance_block();

  ViewGatewayServer server(std::format("localhost:{}",
                                       framework_.get_db_exec_port()),
                           "localhost", framework_.get_event_exec_port());
  uint16_t port = server.start_server(uint16_t{0});
  auto client =
      make_rpc_client<ViewGateway1>(x23::HostPort{"localhost", port})().value();

  // read_value takes no auth handles, so its entrypoint refuses any forwarded
  // token; a token at the chain's depth must get the answer a shallow one does.
  ViewGatewayResult shallow =
      client.viewgateway_view(make_query_forwarding(1))();
  ViewGatewayResult deep =
      client.viewgateway_view(make_query_forwarding(kChainDepth))();
  ASSERT_EQ(shallow.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  ASSERT_EQ(deep.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  EXPECT_EQ(deep.payload().output.status_code(),
            shallow.payload().output.status_code());

  ViewGatewayResult too_deep =
      client.viewgateway_view(make_query_forwarding(kChainDepth + 1))();
  ASSERT_EQ(too_deep.outcome(), ViewGatewayOutcome::ViewGatewayInvalidInput);
  EXPECT_EQ(too_deep.error_code(),
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput);

  server.impl()->shut_down();
}

} // namespace

// Interpose on real database reads without adding callbacks to the executor.
class ControlledViewDb : public rpc_client_t<DatabaseToExecutorInterfaceV1> {
  using Client = rpc_client_t<DatabaseToExecutorInterfaceV1>;

public:
  using rpc_service = DatabaseToExecutorInterfaceV1;
  explicit ControlledViewDb(std::uint16_t port)
    : Client(make_rpc_client<DatabaseToExecutorInterfaceV1>(
                 x23::HostPort{"localhost", port})()
                 .value())
  {}

  std::function<void(BlockID, AddressAndKey const &)> before_read;
  std::atomic<std::size_t> snapshots_opened{0};

  x23::Async<ExpectedStorageItem> get_snapshot_item(BlockID id,
                                                    AddressAndKey key)
  {
    if (before_read) {
      co_await default_threadpool().async(before_read, id, std::cref(key));
    }
    co_return co_await Client::get_snapshot_item(id, key);
  }

  x23::Async<ExpectedViewingSnapshot> open_viewing_snapshot()
  {
    ++snapshots_opened;
    co_return co_await Client::open_viewing_snapshot();
  }
};

class ViewGatewayBatchTest : public ViewGatewayRefreshTest {
public:
  static void stop_refresh_worker(ViewGatewayRpc &gateway)
  {
    gateway.refresh_thread_.request_stop();
    gateway.refresh_thread_.join();
  }

  static ViewGatewayBatchQuery batch()
  {
    auto first = normalize_public_view_query(make_read_query());
    first.invocation.gas_limit = 10'000'000;
    auto second = first;
    ContractTraitClient<view_gateway_test::ViewGatewayTest> client(kAddr);
    second.invocation = client.make_invocation<u8"read_value">(
        10'000'000, NativeContractTestFramework::make_storage_key("other_key"));
    return ViewGatewayBatchQuery{.calls = {first, second},
                                 .min_height = {},
                                 .budget_millis = 30'000,
                                 .max_response_bytes = 1'048'576};
  }

  ViewGatewayBatchResult pinned_attempt(ViewGatewayRpc &gateway,
                                        ViewGatewayBatchQuery const &query,
                                        bool commit, bool expired)
  {
    auto *state = gateway.get_current_view_state()();
    CHECK(static_cast<bool>(state));
    const auto id = state->get().block_id;
    if (commit) {
      commit_bytestring("new-value");
      framework_.advance_block();
    }
    auto deadline =
        std::chrono::steady_clock::now() +
        (expired ? std::chrono::seconds(-1) : std::chrono::seconds(30));
    auto result = gateway.run_batch_attempt(query, state->get(), deadline)();
    if (state->release_reference()) {
      gateway.gc_view_state(id)();
    }
    return result;
  }
};

TEST_F(ViewGatewayBatchTest, PreservesClaimsForOrdinaryAndWalletInvocations)
{
  const auto address = NativeContractTestFramework::make_contract_address(42);
  auto contract = ContractTraitMeta<authclaim_test::AuthClaimTest>::load_rv32();
  framework_.write_contract(contract);
  framework_.write_deployment(address, contract);
  commit_bytestring("value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });

  ContractTraitClient<authclaim_test::AuthClaimTest> ordinary(address);
  auto default_claim = make_read_query();
  default_claim.invocation =
      ordinary.make_invocation<u8"test_get_auth_claim">(10'000'000);
  auto nonzero = default_claim;
  nonzero.auth_claim.source_address_claim() = {.allowed_keys = 5,
                                               .required_weight = 3};
  // This existing non-wallet fixture accepts only the zero/default claim.
  EXPECT_EQ(
      gateway.viewgateway_view(default_claim)().payload().output.status_code(),
      InvocationFrameStatusCode::Return);
  EXPECT_EQ(gateway.viewgateway_view(nonzero)().payload().output.status_code(),
            InvocationFrameStatusCode::Fault);

  auto wallet_query = make_query();
  PublicKey owner_key(CryptographicScheme::Ed25519);
  owner_key.ed25519().fill(42);
  wallet_query.auth_claim.pubkey_preimage() =
      make_pubkey_account_preimage(owner_key, /*nonce=*/0);
  const auto owner =
      pubkey_auth_address(wallet_query.auth_claim.pubkey_preimage());
  if (!owner.has_value()) {
    FAIL() << "Expected an Ed25519 authorization address";
  }
  ContractTraitClient<default_wallet::DefaultWallet> wallet(*owner);
  const auto inner = make_read_query().invocation;
  x23::Vec<default_wallet::WalletCall, 0xffffffff> calls{
      {.target = inner.invoked_address,
       .entrypoint = inner.entrypoint,
       .gas_limit = 1'000'000,
       .calldata = inner.calldata}};
  wallet_query.invocation =
      wallet.make_invocation<u8"execute">(10'000'000, calls);
  EXPECT_EQ(
      gateway.viewgateway_view(wallet_query)().payload().output.status_code(),
      InvocationFrameStatusCode::Return);
  auto wrong = wallet_query;
  ++wrong.auth_claim.pubkey_preimage().nonce;
  EXPECT_EQ(gateway.viewgateway_view(wrong)().payload().output.status_code(),
            InvocationFrameStatusCode::Abort);
  // Pubkey claims also reach the ordinary contract, which rejects that variant.
  auto ordinary_pubkey = default_claim;
  ordinary_pubkey.auth_claim = wallet_query.auth_claim;
  EXPECT_EQ(gateway.viewgateway_view(ordinary_pubkey)()
                .payload()
                .output.status_code(),
            InvocationFrameStatusCode::Fault);

  auto nested = wallet_query;
  calls[0].target = address;
  calls[0].entrypoint = default_claim.invocation.entrypoint;
  calls[0].calldata = default_claim.invocation.calldata;
  nested.invocation = wallet.make_invocation<u8"execute">(10'000'000, calls);
  const auto nested_result = gateway.viewgateway_view(nested)();
  ASSERT_EQ(nested_result.payload().output.status_code(),
            InvocationFrameStatusCode::Abort);
  // The wallet accepts the claim; its ordinary child rejects the pubkey
  // variant.
  EXPECT_NE(nested_result.payload().output,
            gateway.viewgateway_view(wrong)().payload().output);

  auto query = batch();
  query.calls = {wallet_query, wrong, default_claim, nonzero};
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  ASSERT_EQ(result.payload().results.size(), 4U);
  EXPECT_EQ(result.payload().results[0].output.status_code(),
            InvocationFrameStatusCode::Return);
  EXPECT_EQ(result.payload().results[1].output.status_code(),
            InvocationFrameStatusCode::Abort);
  EXPECT_EQ(result.payload().results[2].output.status_code(),
            InvocationFrameStatusCode::Return);
  EXPECT_EQ(result.payload().results[3].output.status_code(),
            InvocationFrameStatusCode::Fault);
}

TEST_F(ViewGatewayBatchTest, ReturnsOrderedResultsAtOneHeight)
{
  commit_bytestring("value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  auto query = batch();
  auto first = gateway.viewgateway_view(query.calls[0])();
  auto second = gateway.viewgateway_view(query.calls[1])();
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  ASSERT_EQ(result.payload().results.size(), 2U);
  EXPECT_EQ(result.payload().height, first.payload().height);
  EXPECT_EQ(result.payload().results[0].output, first.payload().output);
  EXPECT_EQ(result.payload().results[0].gas_consumed,
            first.payload().gas_consumed);
  EXPECT_GT(result.payload().results[0].gas_consumed, 0U);
  EXPECT_EQ(result.payload().results[1].output, second.payload().output);
  gateway.shut_down();
}

TEST_F(ViewGatewayBatchTest, IsolatesWritesAndPreservesCallerAndMixedOutcomes)
{
  const auto address = NativeContractTestFramework::make_contract_address(42);
  auto contract =
      ContractTraitMeta<manual_revert_test::ManualRevertTest>::load_rv32();
  framework_.write_contract(contract);
  framework_.write_deployment(address, contract);
  framework_.setup_grant_storage_token(address, 1'000'000'000);
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  ContractTraitClient<manual_revert_test::ManualRevertTest> client(address);
  auto read = make_query();
  read.invocation =
      client.make_invocation<u8"check_storage_exists">(10'000'000);
  auto baseline = gateway.viewgateway_view(read)();
  ASSERT_EQ(baseline.outcome(), ViewGatewayOutcome::ViewGatewayOk);
  ASSERT_EQ(baseline.payload().output.status_code(),
            InvocationFrameStatusCode::Return);
  auto write = read;
  write.invocation =
      client.make_invocation<u8"test_basic_manual_revert">(10'000'000, false);
  ViewGatewayBatchQuery query{.calls = {write, read},
                              .min_height = {},
                              .budget_millis = 30'000,
                              .max_response_bytes = 1'048'576};
  for (uint32_t mode = 0; mode < 3; ++mode) {
    auto call = read;
    call.parent_address =
        NativeContractTestFramework::make_contract_address(100 + mode);
    call.invocation =
        client.make_invocation<u8"test_initializer">(10'000'000,
                                                     *call.parent_address, mode,
                                                     mode);
    query.calls.push_back(std::move(call));
  }
  query.calls.push_back(read);
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  auto const &results = result.payload().results;
  ASSERT_EQ(results.size(), 6U);
  EXPECT_EQ(results[0].output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(results[1].output, baseline.payload().output);
  EXPECT_EQ(results[2].output.status_code(), InvocationFrameStatusCode::Return);
  EXPECT_EQ(results[3].output.status_code(), InvocationFrameStatusCode::Abort);
  EXPECT_EQ(results[4].output.status_code(), InvocationFrameStatusCode::Fault);
  EXPECT_EQ(results[5].output, baseline.payload().output);
  EXPECT_EQ(gateway.viewgateway_view(read)().payload().output,
            baseline.payload().output);
}

TEST_F(ViewGatewayBatchTest, MinimumHeightUsesSharedRefreshWithoutRequestingOne)
{
  commit_bytestring("old-value");
  framework_.advance_block();
  RpcServer<ControlledViewDb> proxy(framework_.get_db_exec_port());
  proxy.start_server(0);
  ViewGatewayRpc gateway(std::format("localhost:{}", proxy.port()), "localhost",
                         framework_.get_event_exec_port());
  stop_refresh_worker(gateway);
  auto warm = gateway.viewgateway_view(make_read_query())();
  const auto height = warm.payload().height;
  commit_bytestring("new-value");
  framework_.advance_block();
  auto query = batch();
  query.min_height = height + 1;
  for (int i = 0; i < 20; ++i) {
    auto unavailable = gateway.viewgateway_view_batch(query)();
    ASSERT_EQ(unavailable.outcome(),
              ViewGatewayBatchOutcome::ViewGatewayBatchHeightUnavailable);
    EXPECT_EQ(unavailable.height_unavailable().observed, height);
  }
  EXPECT_EQ(proxy.impl()->snapshots_opened.load(), 1U);
  gateway.request_refresh();
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  EXPECT_GE(result.payload().height, height + 1);
  EXPECT_NE(result.payload().results[0].output, warm.payload().output);
  query.min_height = height + 100;
  auto unavailable = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(unavailable.outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchHeightUnavailable);
  EXPECT_EQ(unavailable.height_unavailable().requested, height + 100);
  EXPECT_LT(unavailable.height_unavailable().observed, height + 100);
  EXPECT_EQ(gateway.viewgateway_view_batch(batch())().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  gateway.shut_down();
}

TEST_F(ViewGatewayBatchTest, RejectsInvalidBatchBeforeExecution)
{
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  auto invalid = [&](ViewGatewayBatchQuery query) {
    EXPECT_EQ(gateway.viewgateway_view_batch(std::move(query))().outcome(),
              ViewGatewayBatchOutcome::ViewGatewayBatchInvalidRequest);
  };
  auto query = batch();
  query.calls.clear();
  invalid(query);
  query = batch();
  query.budget_millis = 0;
  invalid(query);
  query = batch();
  query.max_response_bytes = 0;
  invalid(query);
  query = batch();
  query.min_height = -1;
  invalid(query);
  query = batch();
  query.calls[0].invocation.gas_limit = 0;
  invalid(query);
  gateway.shut_down();
}

TEST_F(ViewGatewayBatchTest, InvalidCallRejectsTheWholeGroup)
{
  commit_bytestring("value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  auto query = batch();
  query.calls[1].parent_address = kAddr;
  EXPECT_EQ(gateway.viewgateway_view_batch(query)().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchInvalidRequest);
}

TEST_F(ViewGatewayBatchTest, UsesGatewayBudgetsInsteadOfProtocolDefaults)
{
  commit_bytestring("value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  auto query = batch();
  query.calls.resize(17, query.calls[0]);
  query.budget_millis = 60'000;
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  EXPECT_EQ(result.payload().results.size(), 17U);
  query.max_response_bytes = 1;
  EXPECT_EQ(gateway.viewgateway_view_batch(query)().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchResponseTooLarge);
}

TEST_F(ViewGatewayBatchTest, EachCallKeepsItsOwnGasLimit)
{
  commit_bytestring("value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  auto query = batch();
  query.calls[1] = query.calls[0];
  query.calls[0].invocation.gas_limit = 1;
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  ASSERT_EQ(result.payload().results.size(), 2U);
  EXPECT_EQ(result.payload().results[0].output.status_code(),
            InvocationFrameStatusCode::Fault);
  EXPECT_EQ(result.payload().results[0].gas_consumed, 1U);
  EXPECT_EQ(result.payload().results[1].output.status_code(),
            InvocationFrameStatusCode::Return);
  EXPECT_GT(result.payload().results[1].gas_consumed, 1U);
  EXPECT_LE(result.payload().results[1].gas_consumed,
            query.calls[1].invocation.gas_limit);
}

TEST_F(ViewGatewayBatchTest, IncludesEventsInTheAggregateResponseLimit)
{
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  ContractTraitClient<view_gateway_test::ViewGatewayTest> client(kAddr);
  auto call = make_query();
  call.invocation =
      client.make_invocation<u8"emit_events">(10'000'000, uint32_t{64});
  ViewGatewayBatchQuery query{.calls = {call},
                              .min_height = {},
                              .budget_millis = 30'000,
                              .max_response_bytes = 1'048'576};
  auto result = gateway.viewgateway_view_batch(query)();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  ASSERT_EQ(result.payload().results[0].output.status_code(),
            InvocationFrameStatusCode::Return);
  ASSERT_EQ(result.payload().results[0].events.size(), 64U);
  EXPECT_EQ(result.payload().results[0].events[0].contract_address, kAddr);
  query.calls.resize(16, call);
  EXPECT_EQ(gateway.viewgateway_view_batch(query)().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchResponseTooLarge);
  query.max_response_bytes = 2'097'152;
  EXPECT_EQ(gateway.viewgateway_view_batch(query)().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchOk);
}

TEST_F(ViewGatewayBatchTest, DiscardsPartialResultsFromAnAbandonedSnapshot)
{
  commit_bytestring("old-value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  auto warm = gateway.viewgateway_view(make_read_query())();
  // The cached first read succeeds on the old state; the uncached second
  // read sees its invalidation. Hold that state across the commit explicitly.
  auto stale = pinned_attempt(gateway, batch(), true, false);
  EXPECT_EQ(stale.outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchAbandoned);
  auto fresh = gateway.viewgateway_view_batch(batch())();
  ASSERT_EQ(fresh.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  EXPECT_GT(fresh.payload().height, warm.payload().height);
  EXPECT_NE(fresh.payload().results[0].output, warm.payload().output);
  EXPECT_EQ(fresh.payload().results.size(), 2U);
  gateway.shut_down();
}

TEST_F(ViewGatewayBatchTest, DeadlineAndShutdownFailTheWholeGroup)
{
  commit_bytestring("value");
  framework_.advance_block();
  ViewGatewayRpc gateway(std::format("localhost:{}",
                                     framework_.get_db_exec_port()),
                         "localhost", framework_.get_event_exec_port());
  EXPECT_EQ(pinned_attempt(gateway, batch(), false, true).outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchDeadlineExceeded);
  gateway.shut_down();
  EXPECT_EQ(gateway.viewgateway_view_batch(batch())().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchInternalError);
}

TEST_F(ViewGatewayBatchTest, PublicBatchRestartsEveryCallAfterAbandonment)
{
  commit_bytestring("old-value");
  framework_.advance_block();
  RpcServer<ControlledViewDb> proxy(framework_.get_db_exec_port());
  proxy.start_server(0);
  ViewGatewayRpc gateway(std::format("localhost:{}", proxy.port()), "localhost",
                         framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  stop_refresh_worker(gateway);
  std::vector<std::size_t> calls;
  std::vector<BlockID> snapshots;
  proxy.impl()->before_read = [&](BlockID id, AddressAndKey const &key) {
    if (key ==
        NativeContractTestFramework::make_contract_data_addrkey(kAddr,
                                                                "test_key")) {
      calls.push_back(0);
      snapshots.push_back(id);
    }
    else if (key == NativeContractTestFramework::make_contract_data_addrkey(
                        kAddr, "other_key")) {
      calls.push_back(1);
      if (snapshots.size() == 1) {
        commit_bytestring("new-value");
        framework_.advance_block();
      }
    }
  };

  auto result = gateway.viewgateway_view_batch(batch())();
  ASSERT_EQ(result.outcome(), ViewGatewayBatchOutcome::ViewGatewayBatchOk);
  EXPECT_EQ(calls, (std::vector<std::size_t>{0, 1, 0, 1}));
  ASSERT_EQ(snapshots.size(), 2U);
  EXPECT_NE(snapshots[0], snapshots[1]);
  ASSERT_EQ(result.payload().results.size(), 2U);
  auto fresh = gateway.viewgateway_view(make_read_query())();
  EXPECT_EQ(result.payload().height, fresh.payload().height);
  EXPECT_EQ(result.payload().results[0].output, fresh.payload().output);
}

TEST_F(ViewGatewayBatchTest, PublicBatchStopsAfterThreeAbandonedAttempts)
{
  commit_bytestring("value-0");
  framework_.advance_block();
  RpcServer<ControlledViewDb> proxy(framework_.get_db_exec_port());
  proxy.start_server(0);
  ViewGatewayRpc gateway(std::format("localhost:{}", proxy.port()), "localhost",
                         framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  stop_refresh_worker(gateway);
  std::vector<std::size_t> calls;
  std::vector<BlockID> snapshots;
  proxy.impl()->before_read = [&](BlockID id, AddressAndKey const &key) {
    if (key ==
        NativeContractTestFramework::make_contract_data_addrkey(kAddr,
                                                                "test_key")) {
      calls.push_back(0);
      snapshots.push_back(id);
    }
    else if (key == NativeContractTestFramework::make_contract_data_addrkey(
                        kAddr, "other_key")) {
      calls.push_back(1);
      commit_bytestring(std::format("value-{}", snapshots.size()));
      framework_.advance_block();
    }
  };

  auto result = gateway.viewgateway_view_batch(batch())();
  EXPECT_EQ(result.outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchAbandoned);
  EXPECT_EQ(calls, (std::vector<std::size_t>{0, 1, 0, 1, 0, 1}));
  ASSERT_EQ(snapshots.size(), 3U);
  EXPECT_NE(snapshots[1], snapshots[0]);
  EXPECT_NE(snapshots[2], snapshots[1]);
}

TEST_F(ViewGatewayBatchTest, PublicBatchStopsWhenBudgetExpiresAfterFirstCall)
{
  commit_bytestring("value");
  framework_.advance_block();
  RpcServer<ControlledViewDb> proxy(framework_.get_db_exec_port());
  proxy.start_server(0);
  ViewGatewayRpc gateway(std::format("localhost:{}", proxy.port()), "localhost",
                         framework_.get_event_exec_port());
  x23::Defer cleanup([&] { gateway.shut_down(); });
  stop_refresh_worker(gateway);
  std::vector<std::size_t> calls;
  proxy.impl()->before_read = [&](BlockID, AddressAndKey const &key) {
    if (key ==
        NativeContractTestFramework::make_contract_data_addrkey(kAddr,
                                                                "test_key")) {
      calls.push_back(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
    else if (key == NativeContractTestFramework::make_contract_data_addrkey(
                        kAddr, "other_key")) {
      calls.push_back(1);
    }
  };
  auto query = batch();
  query.budget_millis = 1000;
  auto result = gateway.viewgateway_view_batch(query)();
  EXPECT_EQ(result.outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchDeadlineExceeded);
  EXPECT_EQ(calls, (std::vector<std::size_t>{0}));
  proxy.impl()->before_read = {};
  EXPECT_EQ(gateway.viewgateway_view_batch(batch())().outcome(),
            ViewGatewayBatchOutcome::ViewGatewayBatchOk);
}

} // namespace hogd
