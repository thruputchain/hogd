#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#include "built_contracts/htlc.h"
#include "built_contracts/itc_erc20.h"
#include "built_contracts/simple_wallet.h"
#include "contract.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/precompiles/native_contract_builder.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/multiplexed_context.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include <tuple>
#include <unordered_map>
#include <utility>

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)
class HTLCIntegrationTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  HTLCIntegrationTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb),
      rv32_runtime_context_(make_hog_exec_context())
  {}

protected:
  void SetUp() override
  {
    using htlc_meta = ContractTraitMeta<htlc::HTLC>;
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

    // Generate keypair for simple_wallet
    auto [public_key, secret_key] = ed25519_keygen();
    wallet_public_key_ = public_key;
    wallet_secret_key_ = secret_key;

    // Load contracts
    auto htlc_contract = htlc_meta::load_rv32();
    auto erc20_contract = erc20_meta::load_rv32();
    auto wallet_contract = wallet_meta::load_rv32();

    // Deploy contracts
    write_contract(htlc_contract);
    write_contract(erc20_contract);
    write_contract(wallet_contract);
    write_deployment(kHTLCContractAddress, htlc_contract);
    write_deployment(kERC20ContractAddress, erc20_contract);
    write_deployment(kWalletContractAddress, wallet_contract);

    // Grant storage tokens.
    //
    // Each committed transaction now charges the replay-cache cost to its root
    // invoked address, so top up the addresses that commit transactions by the
    // cost of the (worst-case) number of commits they make in any single test.
    // No test here asserts an exact native-token balance, so a slightly
    // generous count only adds spare and keeps every commit succeeding.
    //   - HTLC address commits initialize_htlc plus up to two claim/refund
    //     transactions in a test => N = 3.
    //   - ERC20 address commits only initialize_erc20 => N = 1.
    //   - Wallet address commits initialize_wallet + mint + approve + create
    //     => N = 4.
    setup_grant_storage_token(kHTLCContractAddress,
                              1'000'000'000 + replay_cache_cost_charged(3));
    setup_grant_storage_token(kERC20ContractAddress,
                              1'000'000'000 + replay_cache_cost_charged(1));
    setup_grant_storage_token(kWalletContractAddress,
                              1'000'000'000 + replay_cache_cost_charged(4));
    setup_grant_storage_token(kReceiverAddress, 1'000'000'000);

    // Initialize contracts
    initialize_htlc();
    initialize_erc20();
    initialize_wallet();
  }

  constexpr static ContractAddress kHTLCContractAddress =
      make_contract_address(0);
  constexpr static ContractAddress kERC20ContractAddress =
      make_contract_address(1);
  constexpr static ContractAddress kWalletContractAddress =
      make_contract_address(2);
  constexpr static ContractAddress kReceiverAddress = make_contract_address(3);

  ContractTraitClient<htlc::HTLC> htlc_client_ =
      ContractTraitClient<htlc::HTLC>(kHTLCContractAddress);

  ContractTraitClient<itc_erc20::ERC20> erc20_client_ =
      ContractTraitClient<itc_erc20::ERC20>(kERC20ContractAddress);

  ContractTraitClient<simple_wallet::SimpleWallet> wallet_client_ =
      ContractTraitClient<simple_wallet::SimpleWallet>(kWalletContractAddress);

  std::unique_ptr<RuntimeContext> rv32_runtime_context_;

  Ed25519PublicKey wallet_public_key_;
  Ed25519SecretKey wallet_secret_key_;

  uint64_t nonce_ = 0;

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  static std::tuple<int64_t, uint64_t> make_difference(int128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  // Helper to mint tokens to the wallet
  void mint_tokens_to_wallet(uint128_t amount)
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    Calldata mint_calldata = x23::serialize(
        kERC20ContractAddress, erc20_meta::entrypoint<u8"mint">(),
        GasAmount{UINT64_MAX},
        erc20_meta::make_calldata<u8"mint">(kWalletContractAddress,
                                            make_amount(amount)));
    Ed25519Signature mint_sig =
        ed25519_sign(wallet_secret_key_, mint_calldata).value();

    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = wallet_client_.make_invocation<u8"do_action">(
                UINT64_MAX, mint_sig, mint_calldata),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
    advance_block();
  }

  // Helper to approve tokens from wallet to HTLC contract
  void approve_tokens_for_htlc(int128_t amount)
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    Calldata approve_calldata =
        x23::serialize(kERC20ContractAddress,
                       erc20_meta::entrypoint<u8"adjust_allowance">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<u8"adjust_allowance">(
                           kHTLCContractAddress, make_difference(amount)));
    Ed25519Signature approve_sig =
        ed25519_sign(wallet_secret_key_, approve_calldata).value();

    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = wallet_client_.make_invocation<u8"do_action">(
                UINT64_MAX, approve_sig, approve_calldata),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
    advance_block();
  }

  // Helper to create an HTLC via wallet
  void create_htlc_via_wallet(const SHA256Hash &hash, int128_t amount,
                              BlockHeight expiration_height)
  {
    using htlc_meta = ContractTraitMeta<htlc::HTLC>;

    Calldata create_htlc_calldata =
        x23::serialize(kHTLCContractAddress,
                       htlc_meta::entrypoint<u8"create_htlc">(),
                       GasAmount{UINT64_MAX},
                       htlc_meta::make_calldata<u8"create_htlc">(
                           kReceiverAddress, kERC20ContractAddress,
                           make_difference(amount), expiration_height, hash));
    Ed25519Signature create_sig =
        ed25519_sign(wallet_secret_key_, create_htlc_calldata).value();

    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = wallet_client_.make_invocation<u8"do_action">(
                UINT64_MAX, create_sig, create_htlc_calldata),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
    advance_block();
  }

  // Helper to claim an HTLC - returns whether the claim succeeded
  ExecResult claim_htlc(const SHA256Hash &hash, const SHA256Hash &preimage,
                        bool advance_after = true)
  {
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation =
                htlc_client_.make_invocation<u8"claim_htlc">(UINT64_MAX, hash,
                                                             preimage),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxRootFrameReverted;
    }
    auto commit_res = handler.try_commit_transaction();
    if (commit_res == ExecResult::ExecResultOK) {
      EXPECT_TRUE(
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = handler_tx})());
    }
    if (advance_after) {
      advance_block();
    }
    return commit_res;
  }

  // Helper to refund (claw back) an HTLC - returns the result
  ExecResult refund_htlc(const SHA256Hash &hash, bool advance_after = true)
  {
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation =
                htlc_client_.make_invocation<u8"refund_htlc">(UINT64_MAX, hash),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxRootFrameReverted;
    }
    auto commit_res = handler.try_commit_transaction();
    if (commit_res == ExecResult::ExecResultOK) {
      EXPECT_TRUE(
          handler.emit_events(ExecResult::ExecResultOK,
                              ProposedTransaction{.signed_tx = handler_tx})());
    }
    if (advance_after) {
      advance_block();
    }
    return commit_res;
  }

  static SHA256Hash make_preimage(uint8_t seed)
  {
    SHA256Hash preimage{};
    for (size_t i = 0; i < 32; i++) {
      preimage[i] = static_cast<uint8_t>(seed + i);
    }
    return preimage;
  }

  void initialize_htlc()
  {
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation =
                htlc_client_.make_invocation<u8"initialize">(UINT64_MAX),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

    advance_block();
  }

  void initialize_erc20()
  {
    // wallet contract is set as owner of the ERC20 contract
    // This gives the wallet contract address minting privileges
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = erc20_client_.make_invocation<u8"initialize">(
                UINT64_MAX, kWalletContractAddress),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

    advance_block();
  }

  void initialize_wallet()
  {
    auto handler_tx = SignedTransaction{
        .tx = Transaction{
            .invocation = wallet_client_.make_invocation<u8"initialize">(
                UINT64_MAX, wallet_public_key_),
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx, *rv32_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

    advance_block();
  }

  BlockHeight get_block_height() const
  {
    return get_db_tip().value.proposal_header.block_height;
  }
};

TEST_F(HTLCIntegrationTest, CreateAndClaimHTLC)
{
  // Generate a preimage and compute its hash
  SHA256Hash preimage = make_preimage(0x00);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_height() + 100;

  // Setup: mint and approve tokens, create HTLC
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // Claim HTLC with preimage
  auto claim_result = claim_htlc(hash, preimage);
  ASSERT_EQ(claim_result, ExecResult::ExecResultOK);

  // Verify receiver has the tokens via native contract
  {
    // Create native contract that checks balance
    NativeContractBuilder builder;
    constexpr static int32_t kBalanceCheckerContractId = 100;

    // Capture addresses in local variables for lambda

    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kBalanceCheckerContractId,
        [](NativeSyscallContext &ctx,
           uint128_xdr expected_balance) -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> erc20_client(
              kERC20ContractAddress);
          auto result = erc20_client.template invoke_opt<u8"get_balance_of">(
              ctx, kReceiverAddress);
          if (!result.has_value() || !result.value().has_value() ||
              !result.value().value().has_value()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          if (!result.value().value().value().is_ok()) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          auto balance = result.value().value().value().ok();
          uint128_t balance_unsigned_int =
              uint128_t{std::get<0>(balance)} << 64 | std::get<1>(balance);
          if (balance_unsigned_int != from_xdr(expected_balance)) {
            return InvokeError{InvokeErrorCode::Deterministic};
          }
          return {};
        });

    auto native_contract = builder.build();
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>>
        native_contract_map;
    native_contract_map.emplace(kBalanceCheckerContractId,
                                std::move(native_contract));
    auto native_runtime_context =
        std::make_unique<NativeContext>(std::move(native_contract_map));

    // Get RV32 context and precompiles context
    auto rv32_context = std::make_unique<Rv32Context>();
    auto precompiles_context = make_hog_exec_precompiles_context();

    // Create MultiplexedContext with our native contract and RV32 context
    auto balance_check_context = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kRv32, {rv32_context.release(), true}},
            {ContractType::kNative, {native_runtime_context.get(), false}}});

    // Deploy the native contract
    auto native_contract_xdr = make_native_contract(kBalanceCheckerContractId);
    const ContractAddress kBalanceCheckerAddress = make_contract_address(4);
    write_contract(native_contract_xdr);
    write_deployment(kBalanceCheckerAddress, native_contract_xdr);
    // This address commits exactly one transaction (N = 1) below, which now
    // charges the replay-cache cost.
    setup_grant_storage_token(kBalanceCheckerAddress,
                              1'000'000'000 + replay_cache_cost_charged(1));
    advance_block();

    // Invoke the balance checker native contract
    SignedTransaction handler_tx{
        .tx = Transaction{
            .invocation =
                Invocation{.invoked_address = kBalanceCheckerAddress,
                           .entrypoint = entrypoint_from_method_id(100),
                           .gas_limit = UINT64_MAX,
                           .calldata =
                               x23::serialize(to_xdr(uint128_t{htlc_amount}))},
            .uniquifier =
                Uniquifier{.block_expiry_height =
                               std::numeric_limits<BlockHeight>::max(),
                           .nonce = nonce_++}}};
    auto handler =
        make_new_system_call_handler(handler_tx.tx, *balance_check_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(
        handler.emit_events(ExecResult::ExecResultOK,
                            ProposedTransaction{.signed_tx = handler_tx})());
  }
}

// Test that an HTLC can be refunded after timeout
TEST_F(HTLCIntegrationTest, TimeoutAndRefund)
{
  SHA256Hash preimage = make_preimage(0x01);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  // Set expiration to current block + 2 so we can advance past it
  const BlockHeight expiration_height = get_block_height() + 2;

  // Setup: mint and transfer tokens, create HTLC
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // Advance past expiration
  while (get_block_height() <= expiration_height) {
    advance_block();
  }

  // Refund should succeed after timeout
  auto refund_result = refund_htlc(hash);
  ASSERT_EQ(refund_result, ExecResult::ExecResultOK);
}

// Test that an HTLC cannot be claimed twice in the same block
TEST_F(HTLCIntegrationTest, CannotDoubleClaimInSameBlock)
{
  SHA256Hash preimage = make_preimage(0x02);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_height() + 100;

  // Setup
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // First claim should succeed (don't advance block)
  auto first_claim = claim_htlc(hash, preimage, false);
  ASSERT_EQ(first_claim, ExecResult::ExecResultOK);

  // Second claim in same block should fail
  auto second_claim = claim_htlc(hash, preimage, false);
  ASSERT_NE(second_claim, ExecResult::ExecResultOK);

  advance_block();
}

// Test that an HTLC cannot be claimed after it was already claimed
TEST_F(HTLCIntegrationTest, CannotClaimAfterAlreadyClaimed)
{
  SHA256Hash preimage = make_preimage(0x03);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_height() + 100;

  // Setup
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // First claim should succeed
  auto first_claim = claim_htlc(hash, preimage);
  ASSERT_EQ(first_claim, ExecResult::ExecResultOK);

  // Second claim in later block should fail
  auto second_claim = claim_htlc(hash, preimage);
  ASSERT_NE(second_claim, ExecResult::ExecResultOK);
}

// Test that an HTLC cannot be claimed after it was refunded
TEST_F(HTLCIntegrationTest, CannotClaimAfterRefund)
{
  SHA256Hash preimage = make_preimage(0x04);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_height() + 2;

  // Setup
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // Advance past expiration
  while (get_block_height() <= expiration_height) {
    advance_block();
  }

  // Refund should succeed
  auto refund_result = refund_htlc(hash);
  ASSERT_EQ(refund_result, ExecResult::ExecResultOK);

  // Claim after refund should fail
  auto claim_result = claim_htlc(hash, preimage);
  ASSERT_NE(claim_result, ExecResult::ExecResultOK);
}

// Test that an HTLC cannot be refunded twice
TEST_F(HTLCIntegrationTest, CannotRefundTwice)
{
  SHA256Hash preimage = make_preimage(0x05);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_height() + 2;

  // Setup
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // Advance past expiration
  while (get_block_height() <= expiration_height) {
    advance_block();
  }

  // First refund should succeed
  auto first_refund = refund_htlc(hash);
  ASSERT_EQ(first_refund, ExecResult::ExecResultOK);

  // Second refund should fail
  auto second_refund = refund_htlc(hash);
  ASSERT_NE(second_refund, ExecResult::ExecResultOK);
}

// Test that an HTLC cannot be refunded in the same block it was refunded
TEST_F(HTLCIntegrationTest, CannotDoubleRefundInSameBlock)
{
  SHA256Hash preimage = make_preimage(0x06);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_height() + 2;

  // Setup
  mint_tokens_to_wallet(htlc_amount);
  approve_tokens_for_htlc(htlc_amount);
  create_htlc_via_wallet(hash, htlc_amount, expiration_height);

  // Advance past expiration
  while (get_block_height() <= expiration_height) {
    advance_block();
  }

  // First refund should succeed (don't advance block)
  auto first_refund = refund_htlc(hash, false);
  ASSERT_EQ(first_refund, ExecResult::ExecResultOK);

  // Second refund in same block should fail
  auto second_refund = refund_htlc(hash, false);
  ASSERT_NE(second_refund, ExecResult::ExecResultOK);

  advance_block();
}

// NOLINTEND(bugprone-unchecked-optional-access)
} // namespace hogd
