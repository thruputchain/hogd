// Payment channel integration tests
//
// Tests the full lifecycle of Lightning-style payment channels:
// - Opening channels with two parties
// - Joining and depositing funds
// - Closing channels with signed state
// - Disputing with newer signed states
// - Finalizing after dispute period
//
// Uses two SimpleWallet contracts as the channel parties, plus an ERC20 token.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "built_contracts/itc_erc20.h"
#include "built_contracts/payment_channel.h"
#include "built_contracts/payment_channel.hh"
#include "built_contracts/simple_wallet.h"
#include "exec_result.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"

namespace hogd {

class PaymentChannelIntegrationTest : public ::testing::Test,
                                      public NativeContractTestFramework {
public:
  PaymentChannelIntegrationTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB),
      wasm_runtime_context_(make_hogexec_context())
  {}

protected:
  void SetUp() override
  {
    using pc_meta = ContractTraitMeta<payment_channel::PaymentChannel>;
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    using wallet_meta = ContractTraitMeta<simple_wallet::SimpleWallet>;

    // Generate keypairs for both parties
    auto [pub_a, sec_a] = ed25519_keygen();
    auto [pub_b, sec_b] = ed25519_keygen();
    party_a_pubkey_ = pub_a;
    party_a_secret_ = sec_a;
    party_b_pubkey_ = pub_b;
    party_b_secret_ = sec_b;

    // Load contracts
    auto pc_contract = pc_meta::load_wasm();
    auto erc20_contract = erc20_meta::load_wasm();
    auto wallet_a_contract = wallet_meta::load_wasm();
    auto wallet_b_contract = wallet_meta::load_wasm();

    // Deploy contracts
    write_contract(pc_contract);
    write_contract(erc20_contract);
    write_contract(wallet_a_contract);
    write_contract(wallet_b_contract);
    write_deployment(kPaymentChannelAddress, pc_contract);
    write_deployment(kERC20Address, erc20_contract);
    write_deployment(kWalletAAddress, wallet_a_contract);
    write_deployment(kWalletBAddress, wallet_b_contract);

    // Grant storage tokens
    setup_grant_storage_token(kPaymentChannelAddress, 1'000'000'000);
    setup_grant_storage_token(kERC20Address, 1'000'000'000);
    setup_grant_storage_token(kWalletAAddress, 1'000'000'000);
    setup_grant_storage_token(kWalletBAddress, 1'000'000'000);

    // Initialize contracts
    initialize_payment_channel();
    initialize_erc20();
    initialize_wallet_a();
    initialize_wallet_b();
  }

  constexpr static ContractAddress kPaymentChannelAddress =
      make_contract_address(0);
  constexpr static ContractAddress kERC20Address = make_contract_address(1);
  constexpr static ContractAddress kWalletAAddress = make_contract_address(2);
  constexpr static ContractAddress kWalletBAddress = make_contract_address(3);
  constexpr static uint64_t kTestDisputePeriod = 10;

  ContractTraitClient<payment_channel::PaymentChannel> pc_client_ =
      ContractTraitClient<payment_channel::PaymentChannel>(
          kPaymentChannelAddress);

  ContractTraitClient<itc_erc20::ERC20> erc20_client_ =
      ContractTraitClient<itc_erc20::ERC20>(kERC20Address);

  ContractTraitClient<simple_wallet::SimpleWallet> wallet_a_client_ =
      ContractTraitClient<simple_wallet::SimpleWallet>(kWalletAAddress);

  ContractTraitClient<simple_wallet::SimpleWallet> wallet_b_client_ =
      ContractTraitClient<simple_wallet::SimpleWallet>(kWalletBAddress);

  std::unique_ptr<RuntimeContext> wasm_runtime_context_;

  Ed25519PublicKey party_a_pubkey_;
  Ed25519SecretKey party_a_secret_;
  Ed25519PublicKey party_b_pubkey_;
  Ed25519SecretKey party_b_secret_;

  uint64_t nonce_ = 0;

  static std::tuple<uint64_t, uint64_t> make_amount(uint128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  static std::tuple<int64_t, uint64_t> make_difference(int128_t amount)
  {
    return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
  }

  // Compute channel_id the same way the contract does
  uint64_t compute_channel_id(uint64_t salt) const
  {
    // Channel ID = hash(party_a_dest, party_a_pubkey, party_b_pubkey, token,
    // salt)
    std::vector<uint8_t> hasher_input;
    hasher_input.insert(hasher_input.end(), kWalletAAddress.begin(),
                        kWalletAAddress.end());
    hasher_input.insert(hasher_input.end(), party_a_pubkey_.begin(),
                        party_a_pubkey_.end());
    hasher_input.insert(hasher_input.end(), party_b_pubkey_.begin(),
                        party_b_pubkey_.end());
    hasher_input.insert(hasher_input.end(), kERC20Address.begin(),
                        kERC20Address.end());
    // Add salt as little-endian bytes
    for (size_t i = 0; i < sizeof(salt); ++i) {
      hasher_input.push_back(static_cast<uint8_t>(salt >> (i * 8)));
    }
    auto hash = sha256(hasher_input);
    // Take first 8 bytes as u64 little-endian
    return static_cast<uint64_t>(hash[0]) |
           (static_cast<uint64_t>(hash[1]) << 8) |
           (static_cast<uint64_t>(hash[2]) << 16) |
           (static_cast<uint64_t>(hash[3]) << 24) |
           (static_cast<uint64_t>(hash[4]) << 32) |
           (static_cast<uint64_t>(hash[5]) << 40) |
           (static_cast<uint64_t>(hash[6]) << 48) |
           (static_cast<uint64_t>(hash[7]) << 56);
  }

  // Serialize ChannelState to CXS bytes for signing (must match Rust's
  // serialize_cxs)
  static x23::Vec<uint8_t> serialize_state(uint64_t channel_id,
                                           int128_t balance_a,
                                           int128_t balance_b,
                                           uint64_t state_nonce)
  {
    payment_channel::ChannelState state{
        .channel_id = channel_id,
        .balance_a =
            {
                .unnamed_0 = static_cast<int64_t>(balance_a >> 64),
                .unnamed_1 =
                    static_cast<uint64_t>(balance_a & 0xffffffffffffffffULL),
            },
        .balance_b =
            {
                .unnamed_0 = static_cast<int64_t>(balance_b >> 64),
                .unnamed_1 =
                    static_cast<uint64_t>(balance_b & 0xffffffffffffffffULL),
            },
        .nonce = state_nonce,
    };
    return x23::serialize_cxs(state);
  }

  // Sign a state update with both parties
  std::pair<Ed25519Signature, Ed25519Signature> sign_state(uint64_t channel_id,
                                                           int128_t balance_a,
                                                           int128_t balance_b,
                                                           uint64_t state_nonce)
  {
    auto msg = serialize_state(channel_id, balance_a, balance_b, state_nonce);
    Ed25519Signature sig_a = ed25519_sign(party_a_secret_, msg);
    Ed25519Signature sig_b = ed25519_sign(party_b_secret_, msg);
    return {sig_a, sig_b};
  }

  // Mint tokens to a wallet
  void mint_tokens(const ContractAddress &wallet, uint128_t amount,
                   const Ed25519SecretKey &secret_key)
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    Calldata mint_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"mint">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"mint">(wallet,
                                                         make_amount(amount)));
    Ed25519Signature mint_sig = ed25519_sign(secret_key, mint_calldata);

    auto &wallet_client =
        (wallet == kWalletAAddress) ? wallet_a_client_ : wallet_b_client_;

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_client.make_invocation<"do_action">(
                        UINT64_MAX, mint_sig, mint_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  // Approve payment channel contract to spend tokens from a wallet
  void approve_pc(const ContractAddress &wallet, uint128_t amount,
                  const Ed25519SecretKey &secret_key)
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;

    // Use adjust_allowance to set the allowance (i128 can be positive to
    // increase)
    Calldata approve_calldata =
        x23::serialize(kERC20Address,
                       erc20_meta::entrypoint<"adjust_allowance">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"adjust_allowance">(
                           kPaymentChannelAddress,
                           make_difference(static_cast<int128_t>(amount))));
    Ed25519Signature approve_sig = ed25519_sign(secret_key, approve_calldata);

    auto &wallet_client =
        (wallet == kWalletAAddress) ? wallet_a_client_ : wallet_b_client_;

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_client.make_invocation<"do_action">(
                        UINT64_MAX, approve_sig, approve_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  // Open a channel via wallet A
  uint64_t open_channel(int128_t amount, uint64_t salt = 0)
  {
    using pc_meta = ContractTraitMeta<payment_channel::PaymentChannel>;

    Calldata open_calldata = x23::serialize(
        kPaymentChannelAddress, pc_meta::entrypoint<"open_channel">(),
        GasAmount{UINT64_MAX},
        pc_meta::make_calldata<"open_channel">(party_a_pubkey_, party_b_pubkey_,
                                               kWalletBAddress, kERC20Address,
                                               make_difference(amount), salt));
    Ed25519Signature open_sig = ed25519_sign(party_a_secret_, open_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, open_sig, open_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    EXPECT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    EXPECT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();

    // Compute and return the channel ID (same algorithm as the contract)
    return compute_channel_id(salt);
  }

  // Join a channel via wallet B
  ExecResult join_channel(uint64_t channel_id, int128_t amount,
                          bool advance_after = true)
  {
    using pc_meta = ContractTraitMeta<payment_channel::PaymentChannel>;

    Calldata join_calldata = x23::serialize(
        kPaymentChannelAddress, pc_meta::entrypoint<"join_channel">(),
        GasAmount{UINT64_MAX},
        pc_meta::make_calldata<"join_channel">(channel_id,
                                               make_difference(amount)));
    Ed25519Signature join_sig = ed25519_sign(party_b_secret_, join_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_b_client_.make_invocation<"do_action">(
                        UINT64_MAX, join_sig, join_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxRootFrameReverted;
    }
    auto commit_res = handler.try_commit_transaction();
    if (commit_res == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    }
    if (advance_after) {
      advance_block();
    }
    return commit_res;
  }

  // Close channel with signed state
  ExecResult close_channel(uint64_t channel_id, int128_t balance_a,
                           int128_t balance_b, uint64_t state_nonce,
                           bool advance_after = true)
  {
    auto [sig_a, sig_b] =
        sign_state(channel_id, balance_a, balance_b, state_nonce);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = pc_client_.make_invocation<"close_channel">(
                        UINT64_MAX, channel_id, make_difference(balance_a),
                        make_difference(balance_b), state_nonce, sig_a, sig_b),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxRootFrameReverted;
    }
    auto commit_res = handler.try_commit_transaction();
    if (commit_res == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    }
    if (advance_after) {
      advance_block();
    }
    return commit_res;
  }

  // Dispute with a newer signed state
  ExecResult dispute(uint64_t channel_id, int128_t balance_a,
                     int128_t balance_b, uint64_t state_nonce,
                     bool advance_after = true)
  {
    auto [sig_a, sig_b] =
        sign_state(channel_id, balance_a, balance_b, state_nonce);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = pc_client_.make_invocation<"dispute">(
                        UINT64_MAX, channel_id, make_difference(balance_a),
                        make_difference(balance_b), state_nonce, sig_a, sig_b),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxRootFrameReverted;
    }
    auto commit_res = handler.try_commit_transaction();
    if (commit_res == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    }
    if (advance_after) {
      advance_block();
    }
    return commit_res;
  }

  // Finalize a channel after dispute period
  ExecResult finalize(uint64_t channel_id, bool advance_after = true)
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        pc_client_.make_invocation<"finalize">(UINT64_MAX,
                                                               channel_id),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    if (!res.is_root_success()) {
      return ExecResult::ExecResultTxRootFrameReverted;
    }
    auto commit_res = handler.try_commit_transaction();
    if (commit_res == ExecResult::ExecResultOK) {
      EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    }
    if (advance_after) {
      advance_block();
    }
    return commit_res;
  }

  // Advance multiple blocks
  void advance_blocks(int count)
  {
    for (int i = 0; i < count; i++) {
      advance_block();
    }
  }

  void initialize_payment_channel()
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = pc_client_.make_invocation<"initialize">(
                        UINT64_MAX, kTestDisputePeriod),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 0}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  void initialize_erc20()
  {
    // Wallet A is set as owner of ERC20 (has minting privileges)
    // Both wallets can mint for testing purposes
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = erc20_client_.make_invocation<"initialize">(
                        UINT64_MAX, kWalletAAddress),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 1}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  void initialize_wallet_a()
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        wallet_a_client_.make_invocation<"initialize">(
                            UINT64_MAX, party_a_pubkey_),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 2}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  void initialize_wallet_b()
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        wallet_b_client_.make_invocation<"initialize">(
                            UINT64_MAX, party_b_pubkey_),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 3}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  BlockHeight get_block_height() const { return get_db_tip().height; }
};

// Test opening a channel, joining, and basic close flow
TEST_F(PaymentChannelIntegrationTest, OpenJoinAndClose)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Mint tokens to both wallets
  mint_tokens(kWalletAAddress, deposit_a, party_a_secret_);
  mint_tokens(kWalletAAddress, deposit_b,
              party_a_secret_); // Mint for B via A (owner)

  // Transfer B's tokens to B's wallet first
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress, make_amount(uint128_t{deposit_b})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  // Transfer tokens to payment channel contract
  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b, party_b_secret_);

  // Open channel
  uint64_t channel_id = open_channel(deposit_a);

  // Join channel
  auto join_result = join_channel(channel_id, deposit_b);
  ASSERT_EQ(join_result, ExecResult::ExecResultOK);

  // Close with initial balances (no off-chain payments made)
  auto close_result = close_channel(channel_id, deposit_a, deposit_b, 1);
  ASSERT_EQ(close_result, ExecResult::ExecResultOK);

  // Advance past dispute period
  advance_blocks(kTestDisputePeriod + 1);

  // Finalize
  auto finalize_result = finalize(channel_id);
  ASSERT_EQ(finalize_result, ExecResult::ExecResultOK);
}

// Test dispute mechanism - party B challenges with newer state
TEST_F(PaymentChannelIntegrationTest, DisputeWithNewerState)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 1000;

  // Setup tokens
  mint_tokens(kWalletAAddress, deposit_a + deposit_b, party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress, make_amount(uint128_t{deposit_b})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b, party_b_secret_);

  // Open and join channel
  uint64_t channel_id = open_channel(deposit_a);
  join_channel(channel_id, deposit_b);

  // Party A tries to close with old state (nonce 1): A=1500, B=500
  auto close_result = close_channel(channel_id, 1500, 500, 1);
  ASSERT_EQ(close_result, ExecResult::ExecResultOK);

  // Party B disputes with newer state (nonce 5): A=800, B=1200
  // (B made more payments after that old state)
  auto dispute_result = dispute(channel_id, 800, 1200, 5);
  ASSERT_EQ(dispute_result, ExecResult::ExecResultOK);

  // Advance past dispute period
  advance_blocks(kTestDisputePeriod + 1);

  // Finalize with the newer disputed state
  auto finalize_result = finalize(channel_id);
  ASSERT_EQ(finalize_result, ExecResult::ExecResultOK);
}

// Test that finalize fails before dispute period ends
TEST_F(PaymentChannelIntegrationTest, CannotFinalizeBeforeDisputePeriod)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Setup
  mint_tokens(kWalletAAddress, deposit_a + deposit_b, party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress, make_amount(uint128_t{deposit_b})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b, party_b_secret_);

  uint64_t channel_id = open_channel(deposit_a);
  join_channel(channel_id, deposit_b);
  close_channel(channel_id, deposit_a, deposit_b, 1);

  // Try to finalize before dispute period ends (only advance half the blocks)
  advance_blocks(kTestDisputePeriod / 2);

  auto finalize_result = finalize(channel_id);
  ASSERT_NE(finalize_result, ExecResult::ExecResultOK);
}

// Test that dispute fails after dispute period ends
TEST_F(PaymentChannelIntegrationTest, CannotDisputeAfterDisputePeriod)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Setup
  mint_tokens(kWalletAAddress, deposit_a + deposit_b, party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress, make_amount(uint128_t{deposit_b})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b, party_b_secret_);

  uint64_t channel_id = open_channel(deposit_a);
  join_channel(channel_id, deposit_b);
  close_channel(channel_id, deposit_a, deposit_b, 1);

  // Advance past dispute period
  advance_blocks(kTestDisputePeriod + 1);

  // Try to dispute after period ended
  auto dispute_result = dispute(channel_id, 700, 800, 2);
  ASSERT_NE(dispute_result, ExecResult::ExecResultOK);
}

// Test that dispute with lower/equal nonce fails
TEST_F(PaymentChannelIntegrationTest, CannotDisputeWithOlderState)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Setup
  mint_tokens(kWalletAAddress, deposit_a + deposit_b, party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress, make_amount(uint128_t{deposit_b})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b, party_b_secret_);

  uint64_t channel_id = open_channel(deposit_a);
  join_channel(channel_id, deposit_b);

  // Close with nonce 5
  close_channel(channel_id, deposit_a, deposit_b, 5);

  // Try to dispute with lower nonce (3)
  auto dispute_result = dispute(channel_id, 700, 800, 3);
  ASSERT_NE(dispute_result, ExecResult::ExecResultOK);

  // Try to dispute with equal nonce (5)
  auto dispute_result2 = dispute(channel_id, 700, 800, 5);
  ASSERT_NE(dispute_result2, ExecResult::ExecResultOK);
}

// Test that double finalize fails
TEST_F(PaymentChannelIntegrationTest, CannotDoubleFinalizeInSameBlock)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Setup
  mint_tokens(kWalletAAddress, deposit_a + deposit_b, party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress, make_amount(uint128_t{deposit_b})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b, party_b_secret_);

  uint64_t channel_id = open_channel(deposit_a);
  join_channel(channel_id, deposit_b);
  close_channel(channel_id, deposit_a, deposit_b, 1);

  // Advance past dispute period
  advance_blocks(kTestDisputePeriod + 1);

  // First finalize should succeed (don't advance block)
  auto first_finalize = finalize(channel_id, false);
  ASSERT_EQ(first_finalize, ExecResult::ExecResultOK);

  // Second finalize in same block should fail
  auto second_finalize = finalize(channel_id, false);
  ASSERT_NE(second_finalize, ExecResult::ExecResultOK);

  advance_block();
}

// Test that channel cannot be joined twice
TEST_F(PaymentChannelIntegrationTest, CannotJoinTwice)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Setup
  mint_tokens(kWalletAAddress, deposit_a + (deposit_b * 2), party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress,
                           make_amount(uint128_t{deposit_b * 2})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(
        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b * 2, party_b_secret_);

  uint64_t channel_id = open_channel(deposit_a);

  // First join should succeed
  auto first_join = join_channel(channel_id, deposit_b);
  ASSERT_EQ(first_join, ExecResult::ExecResultOK);

  // Second join should fail
  auto second_join = join_channel(channel_id, deposit_b);
  ASSERT_NE(second_join, ExecResult::ExecResultOK);
}

// Test that two join transactions in the same block - one should fail
TEST_F(PaymentChannelIntegrationTest, TwoJoinTxsInSameBlockOneFails)
{
  const int128_t deposit_a = 1000;
  const int128_t deposit_b = 500;

  // Setup
  mint_tokens(kWalletAAddress, deposit_a + (deposit_b * 2), party_a_secret_);
  {
    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kERC20Address, erc20_meta::entrypoint<"transfer">(),
                       GasAmount{UINT64_MAX},
                       erc20_meta::make_calldata<"transfer">(
                           kWalletBAddress,
                           make_amount(uint128_t{deposit_b * 2})));
    Ed25519Signature transfer_sig =
        ed25519_sign(party_a_secret_, transfer_calldata);

    auto handler = make_new_system_call_handler(

        Transaction{.invocation = wallet_a_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    EXPECT_TRUE(handler.emit_events(ExecResult::ExecResultOK)());
    advance_block();
  }

  approve_pc(kWalletAAddress, deposit_a, party_a_secret_);
  approve_pc(kWalletBAddress, deposit_b * 2, party_b_secret_);

  uint64_t channel_id = open_channel(deposit_a);

  // First join should succeed (don't advance block)
  auto first_join = join_channel(channel_id, deposit_b, false);
  ASSERT_EQ(first_join, ExecResult::ExecResultOK);

  // Second join in same block should fail (channel already joined)
  auto second_join = join_channel(channel_id, deposit_b, false);
  ASSERT_NE(second_join, ExecResult::ExecResultOK);

  advance_block();
}

} // namespace hogd
