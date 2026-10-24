#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "built_contracts/airdrop_faucet.h"
#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
#include "built_contracts/drip_faucet.h"
#include "built_contracts/drip_faucet.hh" // IWYU pragma: keep
#include "built_contracts/faucet_caller.h"
#include "built_contracts/itc_erc20.h"
#include "built_contracts/itc_erc20.hh" // IWYU pragma: keep
#include "contract.hh"
#include "exec_result.hh"
#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/components/contract_result.h"
#include "hogd/hog_exec/components/library_exception.hh"
#include "hogd/hog_exec/precompiles/precompile_default_wallet.h"
#include "hogd/hog_exec/precompiles/precompile_native_token.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "x23/x23.h"

//
// End-to-end faucet scenarios on a chain whose genesis ROOT contract is a
// faucet: the native token's mint is gated on msg_sender == the genesis-root
// address, so a chain that wants open minting (a testnet) installs a faucet
// as its root. The faucet arrives through the real genesis path
// (GenesisConfigurationBuilder + run_genesis_initialization), never via a
// hand-written deployment, and the callers are kPubkeyAuth accounts: their
// addresses derive from a PubkeyAccountPreimage and their transactions run
// through the genesis-installed native default wallet's `execute`.
//

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {

using wallet_meta = ContractTraitMeta<default_wallet::DefaultWallet>;
using airdrop_meta = ContractTraitMeta<airdrop_faucet::AirdropFaucet>;
using drip_meta = ContractTraitMeta<drip_faucet::DripFaucet>;
using caller_meta = ContractTraitMeta<faucet_caller::FaucetCaller>;
using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
using default_wallet::WalletCall;
using wallet_calls_t = x23::Vec<WalletCall, 4294967295>;

std::tuple<int64_t, uint64_t>
make_amount(int128_t amount)
{
  return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
class FaucetScenarioTest : public ::testing::Test,
                           public NativeContractTestFramework {
public:
  FaucetScenarioTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, /*empty_genesis=*/false)
  {}

protected:
  std::unique_ptr<MultiplexedContext> ctx_;

  // Native side serves the two genesis precompiles (token, default wallet);
  // the faucet root and any deployed callers are rv32.
  void make_ctx()
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> native_map;
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractNativeToken),
        make_native_token_contract());
    native_map.emplace(
        std::to_underlying(
            HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet),
        make_native_default_wallet_contract());
    auto native_ctx = std::make_unique<NativeContext>(std::move(native_map));
    auto rv32_ctx = std::make_unique<Rv32Context>();
    ctx_ = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType,
                           std::pair<RuntimeContext const *, bool>>{
            {ContractType::kRv32, {rv32_ctx.release(), true}},
            {ContractType::kNative, {native_ctx.release(), true}},
        });
  }

  // The whole (height-0) genesis ceremony for a faucet-rooted chain: the
  // faucet is the GenesisConfiguration's root contract, installed at the
  // genesis-root address by the real prerequisite loader. This root defines
  // no height-1 distribution transactions, so loading the configuration IS
  // the full ceremony.
  void run_faucet_genesis(std::shared_ptr<const Contract> const &faucet,
                          StorageCostModel const &cost_model)
  {
    GenesisConfigurationBuilder builder;
    run_genesis_initialization(builder.build(NetworkHash{}, cost_model, *faucet,
                                             stock_network_config()),
                               BlockExecutionMode::kBlockExecutionModeProposal);
  }

  // kPubkeyAuth account: the address derives from the pubkey preimage, and
  // every transaction carries a kPubkeySignature claim resolving to it.
  static PubkeyAccountPreimage make_preimage(uint8_t seed)
  {
    PublicKey key(CryptographicScheme::Ed25519);
    key.ed25519().fill(seed);
    return make_pubkey_account_preimage(key, seed);
  }

  static ContractAddress account_address(PubkeyAccountPreimage const &p)
  {
    return pubkey_auth_address(p).value();
  }

  static AuthClaim make_pubkey_claim(PubkeyAccountPreimage const &preimage)
  {
    AuthClaim claim{};
    claim.claim_type(AuthClaimType::kPubkeySignature);
    claim.pubkey_preimage() = preimage;
    return claim;
  }

  static WalletCall make_wallet_call(ContractAddress const &target,
                                     Runtime::MethodID method_id,
                                     Calldata const &calldata)
  {
    WalletCall call{};
    call.target = target;
    call.entrypoint = entrypoint_from_method_id(method_id);
    call.gas_limit = UINT64_MAX;
    call.calldata = calldata;
    return call;
  }

  static WalletCall airdrop_claim_call(int128_t amount)
  {
    return make_wallet_call(kGenesisRootContractAddress,
                            airdrop_meta::method_id<u8"claim">(),
                            airdrop_meta::make_calldata<u8"claim">(
                                make_amount(amount)));
  }

  static WalletCall drip_claim_call(int128_t amount)
  {
    return make_wallet_call(kGenesisRootContractAddress,
                            drip_meta::method_id<u8"claim">(),
                            drip_meta::make_calldata<u8"claim">(
                                make_amount(amount)));
  }

  struct TxOutcome {
    bool root_success{};
    ExecResult commit{};
    InvocationFrameStatusCode status{};
    std::vector<uint8_t> buffer;
  };

  // One root transaction from a pubkey account, through the default-wallet
  // fallback. Does not advance the block, so several account transactions
  // can share one block.
  TxOutcome
  run_account_invocation_no_advance(PubkeyAccountPreimage const &preimage,
                                    Invocation invocation)
  {
    SignedTransaction stx;
    stx.tx.invocation = std::move(invocation);
    stx.tx.auth_claim = make_pubkey_claim(preimage);
    stx.tx.uniquifier = make_new_uniquifier();

    auto handler = make_new_system_call_handler(stx, *ctx_);
    TxOutcome out;
    out.root_success = handler.root_invoke()().is_root_success();
    auto frame_output = handler.get_most_recent_frame_output();
    out.status = frame_output.status_code();
    switch (out.status) {
    case InvocationFrameStatusCode::Return:
      out.buffer.assign(frame_output.return_buffer().begin(),
                        frame_output.return_buffer().end());
      break;
    case InvocationFrameStatusCode::Abort:
      out.buffer.assign(frame_output.abort_buffer().begin(),
                        frame_output.abort_buffer().end());
      break;
    case InvocationFrameStatusCode::Fault:
      break;
    }
    out.commit = handler.try_commit_transaction()();
    if (out.commit == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK,
                                      ProposedTransaction{.signed_tx = stx})());
    }
    return out;
  }

  TxOutcome run_execute_tx_no_advance(PubkeyAccountPreimage const &preimage,
                                      wallet_calls_t const &calls)
  {
    ContractTraitClient<default_wallet::DefaultWallet> client(
        account_address(preimage));
    return run_account_invocation_no_advance(
        preimage, client.make_invocation<u8"execute">(kDefaultMaxTxGas, calls));
  }

  TxOutcome run_execute_tx(PubkeyAccountPreimage const &preimage,
                           wallet_calls_t const &calls)
  {
    auto out = run_execute_tx_no_advance(preimage, calls);
    advance_block();
    return out;
  }

  static wallet_calls_t single_call(WalletCall const &call)
  {
    wallet_calls_t calls;
    calls.push_back(call);
    return calls;
  }

  TxOutcome run_claim_tx(PubkeyAccountPreimage const &preimage,
                         WalletCall const &call)
  {
    return run_execute_tx(preimage, single_call(call));
  }

  TxOutcome run_claim_tx_no_advance(PubkeyAccountPreimage const &preimage,
                                    WalletCall const &call)
  {
    return run_execute_tx_no_advance(preimage, single_call(call));
  }

  int128_t read_balance(ContractAddress const &addr)
  {
    auto item = lookup_storage_item_from_db(
        make_free_native_token_balance_addrkey(addr));
    if (!item.has_value()) {
      return 0;
    }
    return from_xdr(item->nnint());
  }

  int128_t read_total_supply()
  {
    StorageKey key(StorageKeyType::ContractData);
    key.contract_data_key() = kNativeTokenTotalSupplyKey;
    auto item = lookup_storage_item_from_db(
        AddressAndKey{.address = kNativeTokenContractAddress,
                      .key = std::move(key)});
    if (!item.has_value()) {
      return 0;
    }
    return from_xdr(item->nnint());
  }
};

// A testnet airdrop bootstrapped from literally nothing: the chain's genesis
// installs the airdrop faucet as root, and pubkey accounts with no prior
// state fund themselves by claiming -- the minted tokens pay the claiming
// transaction's own storage costs at commit.
TEST_F(FaucetScenarioTest, AirdropBootstrapsPubkeyAccountsFromZero)
{
  make_ctx();
  auto faucet = airdrop_meta::load_rv32();
  ASSERT_TRUE(faucet);
  run_faucet_genesis(faucet, *get_default_cost_model());

  auto alice = make_preimage(1);
  auto bob = make_preimage(2);
  constexpr uint128_t kAliceAmount = 10'000'000;
  constexpr uint128_t kBobAmount = 5'000'000;
  // Minted by the chain's first claim: the airdrop faucet's one-time
  // bootstrap of the token contract's rows and its own (TOKEN_ROW_FUNDING +
  // ROOT_ROW_FUNDING), mirroring the stock ceremony's token_funding and
  // root_funding mints.
  constexpr uint128_t kBootstrapFunding = 2'000'000;

  auto alice_out = run_claim_tx(alice, airdrop_claim_call(kAliceAmount));
  EXPECT_TRUE(alice_out.root_success);
  EXPECT_EQ(alice_out.status, InvocationFrameStatusCode::Return);
  EXPECT_EQ(alice_out.commit, ExecResult::ExecResultOK)
      << "the claim must self-fund the account's storage costs";

  auto bob_out = run_claim_tx(bob, airdrop_claim_call(kBobAmount));
  EXPECT_EQ(bob_out.commit, ExecResult::ExecResultOK);

  // Minted supply is exactly the two claims; each account's balance is its
  // claim minus the storage costs its own transaction consumed.
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>(kAliceAmount + kBobAmount +
                                  kBootstrapFunding));
  auto alice_balance = read_balance(account_address(alice));
  auto bob_balance = read_balance(account_address(bob));
  EXPECT_GT(alice_balance, 0);
  EXPECT_LT(alice_balance, static_cast<int128_t>(kAliceAmount));
  EXPECT_GT(bob_balance, 0);
  EXPECT_LT(bob_balance, static_cast<int128_t>(kBobAmount));
  EXPECT_EQ(static_cast<int128_t>(kAliceAmount) - alice_balance,
            static_cast<int128_t>(kBobAmount) - bob_balance)
      << "both accounts paid identical bootstrap costs";

  // A claim too small to cover its own costs cannot commit, and stages
  // nothing: the account stays empty and no supply is created.
  auto pauper = make_preimage(3);
  auto pauper_out = run_claim_tx(pauper, airdrop_claim_call(1));
  EXPECT_TRUE(pauper_out.root_success);
  EXPECT_NE(pauper_out.commit, ExecResult::ExecResultOK)
      << "a 1-token claim cannot pay for its own storage";
  EXPECT_EQ(read_balance(account_address(pauper)), 0);
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>(kAliceAmount + kBobAmount +
                                  kBootstrapFunding));

  // A negative amount, down to INT128_MIN, is rejected by the native token
  // itself (NegativeAmount); the wallet forwards that exception and nothing
  // is minted.
  auto greedy = make_preimage(4);
  auto greedy_out =
      run_claim_tx(greedy,
                   airdrop_claim_call(std::numeric_limits<int128_t>::min()));
  EXPECT_EQ(greedy_out.status, InvocationFrameStatusCode::Abort);
  auto expected = x23::serialize_cxs(uint64_t{0xf63c8b1f5e7e2fe0ULL},
                                     itc_erc20::ERC20Error::NegativeAmount)
                      .value();
  EXPECT_TRUE(std::ranges::equal(greedy_out.buffer, expected));
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>(kAliceAmount + kBobAmount +
                                  kBootstrapFunding));
}

// Two accounts race the chain's very first claim in ONE block. The bootstrap
// mint that funds the token contract's own rows must land exactly once, so
// the faucet takes an exclusive lock on a bootstrap flag: the two claims
// cannot share a block, one commits (bootstrapping the chain) and the other
// is rejected and retries later. Guarding on the token's balance instead
// would not serialize -- native balance updates are commutative, so both
// transactions would read a zero pre-block balance, both would mint, and the
// chain would end up with twice the bootstrap funding.
TEST_F(FaucetScenarioTest, ConcurrentFirstClaimsBootstrapExactlyOnce)
{
  make_ctx();
  auto faucet = airdrop_meta::load_rv32();
  ASSERT_TRUE(faucet);
  run_faucet_genesis(faucet, *get_default_cost_model());

  auto alice = make_preimage(1);
  auto bob = make_preimage(2);
  constexpr uint128_t kClaim = 10'000'000;
  constexpr uint128_t kBootstrapFunding = 2'000'000;

  auto a = run_claim_tx_no_advance(alice, airdrop_claim_call(kClaim));
  auto b = run_claim_tx_no_advance(bob, airdrop_claim_call(kClaim));
  advance_block();

  // Exactly one bootstraps the chain; the other loses the exclusive lock.
  const bool alice_won = a.commit == ExecResult::ExecResultOK;
  const bool bob_won = b.commit == ExecResult::ExecResultOK;
  EXPECT_NE(alice_won, bob_won)
      << "exactly one of two concurrent first claims may commit";
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>(kClaim + kBootstrapFunding))
      << "the bootstrap funding must be minted exactly once";

  // The loser retries in the next block: the chain is bootstrapped now, so
  // its claim takes the ordinary path and mints only its own amount.
  auto const &loser = alice_won ? bob : alice;
  auto retry = run_claim_tx(loser, airdrop_claim_call(kClaim));
  EXPECT_EQ(retry.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>((2 * kClaim) + kBootstrapFunding));

  // Once bootstrapped, concurrent claims no longer contend at all: both of
  // these commit in the same block.
  auto carol = make_preimage(3);
  auto dave = make_preimage(4);
  auto c = run_claim_tx_no_advance(carol, airdrop_claim_call(kClaim));
  auto d = run_claim_tx_no_advance(dave, airdrop_claim_call(kClaim));
  advance_block();
  EXPECT_EQ(c.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(d.commit, ExecResult::ExecResultOK)
      << "the exclusive lock must only be taken on the bootstrap claim";
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>((4 * kClaim) + kBootstrapFunding));
}

// The drip faucet's global 100-per-5-blocks budget, including concurrent
// claims: consumption is a commutative capacity decrement, so claims from
// different transactions in one block compose, an in-transaction overdraw is
// rejected cleanly by the contract, and a cross-transaction overdraw within
// one block is rejected at commit by the block-packing bound. Storage costs
// are zeroed so 100-unit claims need no bootstrap funding.
TEST_F(FaucetScenarioTest, DripFaucetEnforcesWindowBudgetAcrossCallers)
{
  make_ctx();
  auto faucet = drip_meta::load_rv32();
  ASSERT_TRUE(faucet);
  run_faucet_genesis(faucet, StorageCostModel(StorageCostModelXdr{
                                 .ext = ExtensionPoint(0),
                                 .base_data_item_cost = 0,
                                 .bytestring_cost_per_byte = 0,
                                 .base_contract_deployment_cost = 0,
                                 .contract_deployment_cost_per_byte = 0,
                                 .auth_per_key_cost = 0,
                                 .rcq_cost_per_entry = 0,
                                 .replay_cache_cost_per_entry = 0,
                                 .balance_cost = 0,
                             }));

  auto alice = make_preimage(1);
  auto bob = make_preimage(2);
  auto carol = make_preimage(3);

  // Block h0: Alice's claim starts the window (capacity reset to 100) and
  // takes 60 of it.
  const auto window_start = get_block_number();
  auto a1 = run_claim_tx(alice, drip_claim_call(60));
  EXPECT_EQ(a1.status, InvocationFrameStatusCode::Return);
  ASSERT_EQ(a1.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(read_balance(account_address(alice)), 60);
  EXPECT_EQ(read_total_supply(), 60);

  // Block h0+1, two claims in ONE block: Bob takes 30 (fits) and Carol asks
  // for 20. Each transaction sees 40 remaining at invoke, so both return
  // Ok -- but together they overdraw the window, and the block-packing bound
  // rejects exactly the transaction that no longer fits at commit.
  auto b1 = run_execute_tx_no_advance(bob, [] {
    wallet_calls_t calls;
    calls.push_back(drip_claim_call(30));
    return calls;
  }());
  auto c1 = run_execute_tx_no_advance(carol, [] {
    wallet_calls_t calls;
    calls.push_back(drip_claim_call(20));
    return calls;
  }());
  advance_block();
  EXPECT_EQ(b1.status, InvocationFrameStatusCode::Return);
  EXPECT_EQ(b1.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(c1.status, InvocationFrameStatusCode::Return)
      << "Carol's overdraw is invisible at invoke";
  EXPECT_NE(c1.commit, ExecResult::ExecResultOK)
      << "the block-packing bound must reject the aggregate overdraw";
  EXPECT_EQ(read_balance(account_address(bob)), 30);
  EXPECT_EQ(read_balance(account_address(carol)), 0);
  EXPECT_EQ(read_total_supply(), 90);

  // With 10 left, Carol's 10 succeeds; Alice's 1 aborts with
  // DripError::ExceedsWindowBudget, forwarded unchanged by the wallet.
  auto c2 = run_claim_tx(carol, drip_claim_call(10));
  EXPECT_EQ(c2.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(read_balance(account_address(carol)), 10);
  auto a2 = run_claim_tx(alice, drip_claim_call(1));
  EXPECT_EQ(a2.status, InvocationFrameStatusCode::Abort);
  auto budget_error =
      x23::serialize_cxs(uint64_t{0x950789fa428d60b9ULL},
                         drip_faucet::DripError::ExceedsWindowBudget)
          .value();
  EXPECT_TRUE(std::ranges::equal(a2.buffer, budget_error));
  EXPECT_EQ(read_total_supply(), 100)
      << "the window's budget is exactly exhausted";

  // Advance past the window (5 blocks since the reset), then the full budget
  // is claimable again in one gulp.
  while (get_block_number() < window_start + 5) {
    advance_block();
  }
  auto a3 = run_claim_tx(alice, drip_claim_call(100));
  EXPECT_EQ(a3.status, InvocationFrameStatusCode::Return);
  EXPECT_EQ(a3.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(read_balance(account_address(alice)), 160);
  EXPECT_EQ(read_total_supply(), 200);

  // Same-transaction batch: one wallet execute claiming 60 then 50 in a
  // fresh window. The second claim sees the first's in-transaction
  // consumption, the contract rejects it, and the wallet aborts the whole
  // batch -- so not even the first claim's mint survives.
  for (int i = 0; i < 6; ++i) {
    advance_block();
  }
  auto batch = run_execute_tx(bob, [] {
    wallet_calls_t calls;
    calls.push_back(drip_claim_call(60));
    calls.push_back(drip_claim_call(50));
    return calls;
  }());
  EXPECT_EQ(batch.status, InvocationFrameStatusCode::Abort);
  EXPECT_TRUE(std::ranges::equal(batch.buffer, budget_error));
  EXPECT_EQ(read_balance(account_address(bob)), 30)
      << "the aborted batch must not mint anything";
  EXPECT_EQ(read_total_supply(), 200);
}

// Negative claims abort (NegativeAmount, or overflow for INT128_MIN), leaving
// the window budget untouched: 40 claimable after Alice's 60.
TEST_F(FaucetScenarioTest, DripFaucetNegativeClaimLeavesBudgetUnchanged)
{
  make_ctx();
  auto faucet = drip_meta::load_rv32();
  ASSERT_TRUE(faucet);
  run_faucet_genesis(faucet, StorageCostModel(StorageCostModelXdr{
                                 .ext = ExtensionPoint(0),
                                 .base_data_item_cost = 0,
                                 .bytestring_cost_per_byte = 0,
                                 .base_contract_deployment_cost = 0,
                                 .contract_deployment_cost_per_byte = 0,
                                 .auth_per_key_cost = 0,
                                 .rcq_cost_per_entry = 0,
                                 .replay_cache_cost_per_entry = 0,
                                 .balance_cost = 0,
                             }));

  auto alice = make_preimage(1);
  auto bob = make_preimage(2);
  auto carol = make_preimage(3);

  auto a1 = run_claim_tx(alice, drip_claim_call(60));
  ASSERT_EQ(a1.commit, ExecResult::ExecResultOK);
  ASSERT_EQ(read_total_supply(), 60);

  auto negative_amount_error =
      x23::serialize_cxs(uint64_t{0xf63c8b1f5e7e2fe0ULL},
                         itc_erc20::ERC20Error::NegativeAmount)
          .value();
  auto overflow_error =
      x23::serialize_cxs(kLibraryExceptionSelector,
                         LibraryExn(
                             LibraryExnKind::kLibraryExnNonNegativeIntOverflow))
          .value();
  for (int128_t amount :
       {int128_t{-50}, std::numeric_limits<int128_t>::min()}) {
    auto negative = run_claim_tx(bob, drip_claim_call(amount));
    EXPECT_EQ(negative.status, InvocationFrameStatusCode::Abort);
    auto const &expected = amount == std::numeric_limits<int128_t>::min()
                               ? overflow_error
                               : negative_amount_error;
    EXPECT_TRUE(std::ranges::equal(negative.buffer, expected));
    EXPECT_EQ(read_balance(account_address(bob)), 0);
    EXPECT_EQ(read_total_supply(), 60);
  }

  // Exactly 40 remain: a claim of 40 fits, and one more token does not.
  auto c1 = run_claim_tx(carol, drip_claim_call(40));
  EXPECT_EQ(c1.status, InvocationFrameStatusCode::Return);
  EXPECT_EQ(c1.commit, ExecResult::ExecResultOK);
  EXPECT_EQ(read_balance(account_address(carol)), 40);
  auto c2 = run_claim_tx(carol, drip_claim_call(1));
  EXPECT_EQ(c2.status, InvocationFrameStatusCode::Abort);
  auto budget_error =
      x23::serialize_cxs(uint64_t{0x950789fa428d60b9ULL},
                         drip_faucet::DripError::ExceedsWindowBudget)
          .value();
  EXPECT_TRUE(std::ranges::equal(c2.buffer, budget_error));
  EXPECT_EQ(read_total_supply(), 100);
}

// The full bootstrap loop: a pubkey account funds itself from the airdrop,
// deploys a contract through the default wallet with those funds, and that
// contract then claims from the faucet for itself -- three ways of acquiring
// native token on a chain that started with zero supply, none of them
// hand-wired by the test.
TEST_F(FaucetScenarioTest, AirdropFundsWalletDeployedContractCaller)
{
  make_ctx();
  auto faucet = airdrop_meta::load_rv32();
  ASSERT_TRUE(faucet);
  run_faucet_genesis(faucet, *get_default_cost_model());

  auto alice = make_preimage(1);
  constexpr uint128_t kAliceAmount = 50'000'000;
  constexpr uint128_t kBootstrapFunding = 2'000'000;
  auto claimed = run_claim_tx(alice, airdrop_claim_call(kAliceAmount));
  ASSERT_EQ(claimed.commit, ExecResult::ExecResultOK);

  // The caller contract's code record must exist in the store before anyone
  // can deploy it by hash (like publishing the code to the network).
  auto caller_obj = caller_meta::load_rv32();
  ASSERT_TRUE(caller_obj);
  write_contract(caller_obj);

  // The deployment's own rows are billed to the deployed address, so Alice
  // funds the (predictable) deploy address first -- the fund-then-deploy
  // order the stock ceremony and deploy_and_transfer both use. The address
  // derivation includes the deployer, which here is Alice's account.
  SHA256Hash nonce{};
  nonce.fill(0x5A);
  ContractAddress predicted_addr =
      SystemCallHandler::static_get_deploy_address(account_address(alice),
                                                   nonce, sha256(*caller_obj));
  constexpr uint128_t kDeployFunding = 5'000'000;
  auto funded =
      run_claim_tx(alice,
                   make_wallet_call(kNativeTokenContractAddress,
                                    erc20_meta::method_id<u8"transfer">(),
                                    erc20_meta::make_calldata<u8"transfer">(
                                        predicted_addr,
                                        make_amount(kDeployFunding))));
  ASSERT_EQ(funded.commit, ExecResult::ExecResultOK);

  // Alice deploys the caller through her wallet, paying from her airdrop.
  ContractTraitClient<default_wallet::DefaultWallet> wallet_client(
      account_address(alice));
  auto deploy_out = run_account_invocation_no_advance(
      alice, wallet_client.make_invocation<u8"deploy">(
                 kDefaultMaxTxGas, nonce, sha256(*caller_obj), make_amount(0),
                 x23::Ptr<default_wallet::WalletInitCall>{}));
  advance_block();
  ASSERT_EQ(deploy_out.status, InvocationFrameStatusCode::Return);
  ASSERT_EQ(deploy_out.commit, ExecResult::ExecResultOK);
  auto deploy_result = wallet_meta::decode_return<u8"deploy">(
      std::as_bytes(
          std::span(deploy_out.buffer.data(), deploy_out.buffer.size())),
      /*manually_reverted=*/false);
  ASSERT_TRUE(deploy_result.has_value());
  ASSERT_TRUE(deploy_result->is_ok());
  ContractAddress caller_addr = deploy_result->ok();
  EXPECT_EQ(caller_addr, predicted_addr);

  // Anyone can now drive the deployed caller; it claims for itself, so the
  // mint lands on the contract's own address.
  constexpr uint128_t kCallerAmount = 20'000'000;
  auto contract_claim =
      run_claim_tx(alice,
                   make_wallet_call(caller_addr,
                                    caller_meta::method_id<u8"claim_from">(),
                                    caller_meta::make_calldata<u8"claim_from">(
                                        kGenesisRootContractAddress,
                                        make_amount(kCallerAmount))));
  EXPECT_EQ(contract_claim.status, InvocationFrameStatusCode::Return);
  EXPECT_EQ(contract_claim.commit, ExecResult::ExecResultOK);

  auto caller_balance = read_balance(caller_addr);
  EXPECT_GT(caller_balance, 0);
  EXPECT_LE(caller_balance,
            static_cast<int128_t>(kCallerAmount + kDeployFunding));
  EXPECT_EQ(read_total_supply(),
            static_cast<int128_t>(kAliceAmount + kCallerAmount +
                                  kBootstrapFunding));
}

// NOLINTEND(bugprone-unchecked-optional-access)
// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
