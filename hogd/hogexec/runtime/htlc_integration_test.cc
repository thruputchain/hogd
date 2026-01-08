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
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/precompiles/native_contract_builder.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/serialize.h"
#include <unordered_map>
#include <utility>

namespace hogd {

class HTLCIntegrationTest : public ::testing::Test,
                            public NativeContractTestFramework {
public:
  HTLCIntegrationTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB),
      wasm_runtime_context_(make_hogexec_context())
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
    auto htlc_contract = htlc_meta::load_wasm();
    auto erc20_contract = erc20_meta::load_wasm();
    auto wallet_contract = wallet_meta::load_wasm();

    // Deploy contracts
    write_contract(htlc_contract);
    write_contract(erc20_contract);
    write_contract(wallet_contract);
    write_deployment(kHTLCContractAddress, htlc_contract);
    write_deployment(kERC20ContractAddress, erc20_contract);
    write_deployment(kWalletContractAddress, wallet_contract);

    // Grant storage tokens
    setup_grant_storage_token(kHTLCContractAddress, 1'000'000'000);
    setup_grant_storage_token(kERC20ContractAddress, 1'000'000'000);
    setup_grant_storage_token(kWalletContractAddress, 1'000'000'000);
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

  std::unique_ptr<RuntimeContext> wasm_runtime_context_;

  Ed25519PublicKey wallet_public_key_;
  Ed25519SecretKey wallet_secret_key_;

  void initialize_htlc()
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        htlc_client_.make_invocation<"initialize">(UINT64_MAX),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 0}}},
        *wasm_runtime_context_);
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
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation = erc20_client_.make_invocation<"initialize">(
                        UINT64_MAX, kWalletContractAddress),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 1}}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

    advance_block();
  }

  void initialize_wallet()
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation = wallet_client_.make_invocation<"initialize">(
                        UINT64_MAX, wallet_public_key_),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 2}}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

    advance_block();
  }
};

TEST_F(HTLCIntegrationTest, CreateAndClaimHTLC)
{
  using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
  using htlc_meta = ContractTraitMeta<htlc::HTLC>;

  // Generate a preimage and compute its hash
  constexpr uint8_t kPreimageBytes[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                          0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
                                          0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
                                          0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                          0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
                                          0x1f, 0x20};
  SHA256Hash preimage;
  std::memcpy(preimage.data(), kPreimageBytes, 32);
  SHA256Hash hash = sha256(preimage);

  const int128_t htlc_amount = 1000;
  const BlockHeight expiration_height = get_block_number() + 100;

  // Mint tokens to wallet via wallet's do_action (wallet is the ERC20 owner)
  // Create internal calldata for the mint
  Calldata mint_calldata = x23::serialize(
      kERC20ContractAddress, erc20_meta::entrypoint<"mint">(),
      GasAmount{UINT64_MAX},
      erc20_meta::make_calldata<"mint">(kWalletContractAddress,
                                        to_xdr(uint128_t{htlc_amount})));
  // Sign the calldata
  Ed25519Signature mint_sig = ed25519_sign(wallet_secret_key_, mint_calldata);
  // Call wallet's do_action to mint tokens
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation = wallet_client_.make_invocation<"do_action">(
                        UINT64_MAX, mint_sig, mint_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 3}}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events()());

    advance_block();
  }

  // Transfer tokens from wallet to HTLC contract via wallet's do_action
  // Create internal calldata for the transfer
  Calldata transfer_calldata = x23::serialize(
      kERC20ContractAddress, erc20_meta::entrypoint<"transfer">(),
      GasAmount{UINT64_MAX},
      erc20_meta::make_calldata<"transfer">(kHTLCContractAddress,
                                            to_xdr(uint128_t{htlc_amount})));
  // Sign the calldata
  Ed25519Signature transfer_sig =
      ed25519_sign(wallet_secret_key_, transfer_calldata);
  // Call wallet's do_action to transfer tokens
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation = wallet_client_.make_invocation<"do_action">(
                        UINT64_MAX, transfer_sig, transfer_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 4}}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events()());

    advance_block();
  }

  // Create HTLC via wallet's do_action
  // Create internal calldata for create_htlc
  Calldata create_htlc_calldata =
      x23::serialize(kHTLCContractAddress,
                     htlc_meta::entrypoint<"create_htlc">(),
                     GasAmount{UINT64_MAX},
                     htlc_meta::make_calldata<"create_htlc">(
                         kReceiverAddress, kERC20ContractAddress,
                         to_xdr(htlc_amount), expiration_height, hash));
  // Sign the calldata
  Ed25519Signature create_sig =
      ed25519_sign(wallet_secret_key_, create_htlc_calldata);
  // Call wallet's do_action to create HTLC
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation = wallet_client_.make_invocation<"do_action">(
                        UINT64_MAX, create_sig, create_htlc_calldata),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 5}}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events()());

    advance_block();
  }

  // Claim HTLC with preimage
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation = htlc_client_.make_invocation<"claim_htlc">(
                        UINT64_MAX, hash, preimage),
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 6}}},
        *wasm_runtime_context_);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events()());

    advance_block();
  }

  // Verify receiver has the tokens via native contract
  {
    // Create native contract that checks balance
    NativeContractBuilder builder;
    constexpr static int32_t kBalanceCheckerContractId = 100;

    // Capture addresses in local variables for lambda

    builder.add<NativeContractBuilder::kDefaultSerializationFormat>(
        kBalanceCheckerContractId,
        [](SystemCallHandler &handler,
           uint128_xdr expected_balance) -> InvokeStatus<void> {
          ContractTraitClient<itc_erc20::ERC20> erc20_client(
              kERC20ContractAddress);
          auto [result, _gas] =
              erc20_client.template invoke<"get_balance_of">(handler,
                                                             kReceiverAddress);
          if (!result.has_value() || !result.value().has_value()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          if (!result.value().value().is_ok()) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
          }
          uint128_xdr balance = result.value().value().ok();
          if (balance != expected_balance) {
            return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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

    // Get WASM context and precompiles context
    auto wasm_context = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
    auto precompiles_context = make_hogexec_precompiles_context();

    // Create MultiplexedContext with our native contract and WASM context
    auto balance_check_context = std::make_unique<MultiplexedContext>(
        std::unordered_map<ContractType, std::pair<RuntimeContext *, bool>>{
            {ContractType::kWasmi, {wasm_context.release(), true}},
            {ContractType::kNative, {native_runtime_context.get(), false}}});

    // Deploy the native contract
    auto native_contract_xdr = make_native_contract(kBalanceCheckerContractId);
    const ContractAddress kBalanceCheckerAddress = make_contract_address(4);
    write_contract(native_contract_xdr);
    write_deployment(kBalanceCheckerAddress, native_contract_xdr);
    setup_grant_storage_token(kBalanceCheckerAddress, 1'000'000'000);
    advance_block();

    // Invoke the balance checker native contract
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = kBalanceCheckerAddress,
                                   .entrypoint = entrypoint_from_method_id(100),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = x23::serialize(
                                       to_xdr(uint128_t{htlc_amount}))},
                    .uniquifier =
                        Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = 7}}},
        *balance_check_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    ASSERT_TRUE(handler.emit_events()());
  }
}

} // namespace hogd
