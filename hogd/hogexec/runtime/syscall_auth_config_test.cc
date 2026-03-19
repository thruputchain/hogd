#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/rust/contract_types/bindings.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

class SyscallAuthConfigTest : public ::testing::Test,
                              public NativeContractTestFramework {
public:
  SyscallAuthConfigTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB)
  {}
};

// Test WriteAddressNonceConfig_inner with a valid nonce config
TEST_F(SyscallAuthConfigTest, WriteAddressNonceConfigSuccess)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             generated::NonceConfig ffi_nonce_config{};
             ffi_nonce_config.accepted_keys = 0x01; // Accept key 0
             ffi_nonce_config.required_weight = Weight{1};

             auto status = ctx.syscall<SyscallNo::kWriteAddressNonceConfig>(
                 ffi_nonce_config);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Verify the nonce config was written
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_EQ(auth_data.nonce_config.accepted_keys, 0x01);
  ASSERT_EQ(auth_data.nonce_config.required_weight, Weight{1});
}

// Test WriteAddressGasPermissionConfigs_inner with valid configs (no gaps)
TEST_F(SyscallAuthConfigTest, WriteAddressGasPermissionConfigsSuccess)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             generated::GasPermissionConfig ffi_config0{};
             ffi_config0.limit = GasAmount{1000};
             ffi_config0.accepted_keys = 0x01; // Accept key 0
             ffi_config0.required_weight = Weight{1};

             generated::GasPermissionConfig ffi_config1{};
             ffi_config1.limit = GasAmount{2000};
             ffi_config1.accepted_keys = 0x02; // Accept key 1
             ffi_config1.required_weight = Weight{2};

             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), &ffi_config0, &ffi_config1, nullptr,
                     nullptr, nullptr);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Verify the gas permission configs were written
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_EQ(auth_data.gas_permissions.size(), 2);
  ASSERT_EQ(auth_data.gas_permissions[0].limit, GasAmount{1000});
  ASSERT_EQ(auth_data.gas_permissions[0].accepted_keys, 0x01);
  ASSERT_EQ(auth_data.gas_permissions[0].required_weight, Weight{1});
  ASSERT_EQ(auth_data.gas_permissions[1].limit, GasAmount{2000});
  ASSERT_EQ(auth_data.gas_permissions[1].accepted_keys, 0x02);
  ASSERT_EQ(auth_data.gas_permissions[1].required_weight, Weight{2});
}

// Test WriteAddressGasPermissionConfigs_inner with empty configs
TEST_F(SyscallAuthConfigTest, WriteAddressGasPermissionConfigsEmpty)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // All configs are null - should succeed with empty list
             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), nullptr, nullptr, nullptr, nullptr,
                     nullptr);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Verify the gas permission configs are empty
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_EQ(auth_data.gas_permissions.size(), 0);
}

// Test WriteAddressGasPermissionConfigs_inner with a gap (null in the middle)
// This should fail with kSyscallErrorCodeInvalidSyscallInput
TEST_F(SyscallAuthConfigTest, WriteAddressGasPermissionConfigsGapError)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             generated::GasPermissionConfig ffi_config0{};
             ffi_config0.limit = GasAmount{1000};
             ffi_config0.accepted_keys = 0x01;
             ffi_config0.required_weight = Weight{1};

             generated::GasPermissionConfig ffi_config2{};
             ffi_config2.limit = GasAmount{2000};
             ffi_config2.accepted_keys = 0x02;
             ffi_config2.required_weight = Weight{2};

             // Gap: config0 is set, config1 is null, config2 is set - should
             // fail
             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), &ffi_config0, nullptr, &ffi_config2,
                     nullptr, nullptr);

             if (!status.has_value() ||
                 status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             // Expected error - return success to allow transaction to complete
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());
}

// Test WriteAddressGasPermissionConfigs_inner with a gap at the end
// (configs followed by nulls should be fine, but nulls followed by configs
// should fail)
TEST_F(SyscallAuthConfigTest, WriteAddressGasPermissionConfigsGapAtEnd)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             generated::GasPermissionConfig ffi_config0{};
             ffi_config0.limit = GasAmount{1000};
             ffi_config0.accepted_keys = 0x01;
             ffi_config0.required_weight = Weight{1};

             // Configs followed by nulls should be fine
             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), &ffi_config0, nullptr, nullptr, nullptr,
                     nullptr);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test WriteAddressGasPermissionConfigs_inner with all 5 configs filled
TEST_F(SyscallAuthConfigTest, WriteAddressGasPermissionConfigsAllFive)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             generated::GasPermissionConfig ffi_configs[5];
             for (int i = 0; i < 5; i++) {
               ffi_configs[i].limit =
                   GasAmount{static_cast<uint64_t>(1000 + (i * 100))};
               ffi_configs[i].accepted_keys = BitMap{1U << i};
               ffi_configs[i].required_weight =
                   Weight{static_cast<uint32_t>(i + 1)};
             }

             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), &ffi_configs[0], &ffi_configs[1],
                     &ffi_configs[2], &ffi_configs[3], &ffi_configs[4]);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Verify all 5 gas permission configs were written
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_EQ(auth_data.gas_permissions.size(), 5);
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(auth_data.gas_permissions[i].limit,
              GasAmount{static_cast<uint64_t>(1000 + (i * 100))});
    ASSERT_EQ(auth_data.gas_permissions[i].accepted_keys, BitMap{1U << i});
    ASSERT_EQ(auth_data.gas_permissions[i].required_weight,
              Weight{static_cast<uint32_t>(i + 1)});
  }
}

// Test calling all three auth config syscalls: ChangePublicKeyRegistration,
// WriteAddressNonceConfig, and WriteAddressGasPermissionConfigs
TEST_F(SyscallAuthConfigTest, AllThreeAuthConfigSyscalls)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Generate an Ed25519 key pair
  auto [public_key, _] = ed25519_keygen();

  // Create a native contract with a method that calls all three syscalls
  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [public_key](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // 1. Set a public key at index 0
             int32_t key_scheme =
                 std::to_underlying(CryptographicScheme::Ed25519);
             uint32_t key_weight = 5;
             std::span<const std::byte> key_bytes(
                 reinterpret_cast<const std::byte *>(public_key.data()),
                 public_key.size());

             auto pk_status =
                 ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                     0, &key_scheme, key_bytes, &key_weight);

             if (!pk_status.has_value() ||
                 pk_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // 2. Set nonce config
             generated::NonceConfig ffi_nonce_config{};
             ffi_nonce_config.accepted_keys = 0x01; // Accept key 0
             ffi_nonce_config.required_weight = Weight{3};

             auto nonce_status =
                 ctx.syscall<SyscallNo::kWriteAddressNonceConfig>(
                     ffi_nonce_config);

             if (!nonce_status.has_value() ||
                 nonce_status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // 3. Set gas permission configs
             generated::GasPermissionConfig ffi_config0{};
             ffi_config0.limit = GasAmount{5000};
             ffi_config0.accepted_keys = 0x01; // Accept key 0
             ffi_config0.required_weight = Weight{3};

             generated::GasPermissionConfig ffi_config1{};
             ffi_config1.limit = GasAmount{10000};
             ffi_config1.accepted_keys = 0x01; // Accept key 0
             ffi_config1.required_weight = Weight{5};

             auto [gas_status, gas_gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), &ffi_config0, &ffi_config1, nullptr,
                     nullptr, nullptr);

             if (!gas_status.has_value() ||
                 gas_status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  // Invoke the contract method which calls all three syscalls
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Verify all three configs were written
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();

  // Verify public key was set
  ASSERT_TRUE(auth_data.public_keys[0]);
  ASSERT_EQ(auth_data.public_keys[0]->key.ed25519(), public_key);
  ASSERT_EQ(auth_data.public_keys[0]->max_weight, Weight{5});

  // Verify nonce config was set
  ASSERT_EQ(auth_data.nonce_config.accepted_keys, 0x01);
  ASSERT_EQ(auth_data.nonce_config.required_weight, Weight{3});

  // Verify gas permission configs were set
  ASSERT_EQ(auth_data.gas_permissions.size(), 2);
  ASSERT_EQ(auth_data.gas_permissions[0].limit, GasAmount{5000});
  ASSERT_EQ(auth_data.gas_permissions[0].accepted_keys, 0x01);
  ASSERT_EQ(auth_data.gas_permissions[0].required_weight, Weight{3});
  ASSERT_EQ(auth_data.gas_permissions[1].limit, GasAmount{10000});
  ASSERT_EQ(auth_data.gas_permissions[1].accepted_keys, 0x01);
  ASSERT_EQ(auth_data.gas_permissions[1].required_weight, Weight{5});
}

// Test WriteAddressNonceConfig_inner rejects invalid accepted_keys bitmap
TEST_F(SyscallAuthConfigTest, WriteAddressNonceConfigInvalidBitmap)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // Bit set beyond kMaxSigners (bit 21 set, kMaxSigners=21)
             generated::NonceConfig ffi_nonce_config{};
             ffi_nonce_config.accepted_keys = 1U << kMaxSigners;
             ffi_nonce_config.required_weight = Weight{1};

             auto status = ctx.syscall<SyscallNo::kWriteAddressNonceConfig>(
                 ffi_nonce_config);
             if (!status.has_value() ||
                 status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE()
                   << "Bitmap with bit beyond kMaxSigners should be rejected";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // All bits set - should also fail
             ffi_nonce_config.accepted_keys = 0xFFFF'FFFF;
             auto status2 = ctx.syscall<SyscallNo::kWriteAddressNonceConfig>(
                 ffi_nonce_config);
             if (!status2.has_value() ||
                 status2.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE() << "Bitmap 0xFFFFFFFF should be rejected";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             // Valid bitmap (highest valid bit) should succeed
             ffi_nonce_config.accepted_keys = 1U << (kMaxSigners - 1);
             auto status3 = ctx.syscall<SyscallNo::kWriteAddressNonceConfig>(
                 ffi_nonce_config);
             if (!status3.has_value() ||
                 status3.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Valid bitmap should succeed";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test WriteAddressGasPermissionConfigs_inner rejects invalid accepted_keys
// bitmap
TEST_F(SyscallAuthConfigTest, WriteAddressGasPermissionConfigsInvalidBitmap)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // First config valid, second has invalid bitmap
             generated::GasPermissionConfig valid_config{};
             valid_config.limit = GasAmount{1000};
             valid_config.accepted_keys = 0x01;
             valid_config.required_weight = Weight{1};

             generated::GasPermissionConfig invalid_config{};
             invalid_config.limit = GasAmount{2000};
             invalid_config.accepted_keys = 1U << kMaxSigners;
             invalid_config.required_weight = Weight{1};

             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     ctx.handler(), &valid_config, &invalid_config, nullptr,
                     nullptr, nullptr);
             if (!status.has_value() ||
                 status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE() << "Gas config with invalid bitmap should be "
                                "rejected";
               return InvokeError{InvokeErrorCode::Deterministic};
             }

             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test ChangePublicKeyRegistration rejects mixed-null args:
// scheme=null but weight!=null
TEST_F(SyscallAuthConfigTest, ChangePublicKeyRegistrationMixedNullSchemeNull)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // scheme=null, weight!=null — should be rejected
             uint32_t key_weight = 5;
             auto status = ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                 0, static_cast<const int32_t *>(nullptr),
                 std::span<const std::byte>{}, &key_weight);
             if (!status.has_value() ||
                 status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE() << "scheme=null, weight!=null should be rejected";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test ChangePublicKeyRegistration rejects mixed-null args:
// scheme=null but bytes not empty
TEST_F(SyscallAuthConfigTest, ChangePublicKeyRegistrationMixedNullBytesNotEmpty)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [public_key, _] = ed25519_keygen();

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          {0, [public_key](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             // scheme=null, bytes not empty — should be rejected
             std::span<const std::byte> key_bytes(
                 reinterpret_cast<const std::byte *>(public_key.data()),
                 public_key.size());
             auto status = ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                 0, static_cast<const int32_t *>(nullptr), key_bytes,
                 static_cast<const uint32_t *>(nullptr));
             if (!status.has_value() ||
                 status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               ADD_FAILURE()
                   << "scheme=null, bytes not empty should be rejected";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  auto handler = make_new_system_call_handler(
      SignedTransaction{
          .tx = Transaction{.invocation =
                                Invocation{.invoked_address = contract_addr,
                                           .entrypoint =
                                               entrypoint_from_method_id(0),
                                           .gas_limit = UINT64_MAX,
                                           .calldata = {}}}},
      *context);

  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
}

// Test ChangePublicKeyRegistration key deletion:
// Register a key, then delete it via all-null args, verify key removed from DB
TEST_F(SyscallAuthConfigTest, ChangePublicKeyRegistrationDeletion)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [public_key, _] = ed25519_keygen();

  auto native_contract = std::make_unique<NativeContract>(
      std::unordered_map<Runtime::MethodID, NativeContract::function_t>{
          // Method 0: Register a key
          {0,
           [public_key](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             int32_t key_scheme =
                 std::to_underlying(CryptographicScheme::Ed25519);
             uint32_t key_weight = 5;
             std::span<const std::byte> key_bytes(
                 reinterpret_cast<const std::byte *>(public_key.data()),
                 public_key.size());
             auto status = ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                 0, &key_scheme, key_bytes, &key_weight);
             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Failed to register key";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }},
          // Method 1: Delete the key via all-null args
          {1, [](NativeSyscallContext &ctx) -> InvokeStatus<void> {
             auto status = ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                 0, static_cast<const int32_t *>(nullptr),
                 std::span<const std::byte>{},
                 static_cast<const uint32_t *>(nullptr));
             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               ADD_FAILURE() << "Failed to delete key";
               return InvokeError{InvokeErrorCode::Deterministic};
             }
             return {};
           }}});

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  // Step 1: Register the key
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(0),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        *context);

    auto root_res = handler.root_invoke();
    ASSERT_TRUE(root_res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  // Verify key was registered
  {
    auto item = lookup_storage_item_from_db(
        make_address_auth_data_addrkey(contract_addr));
    ASSERT_TRUE(item.has_value());
    auto auth_data = item->auth_data();
    ASSERT_TRUE(auth_data.public_keys[0]);
    ASSERT_EQ(auth_data.public_keys[0]->key.ed25519(), public_key);
  }

  // Step 2: Delete the key
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(1),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}}}},
        *context);

    auto root_res = handler.root_invoke();
    ASSERT_TRUE(root_res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_EQ(commit_res, ExecResult::ExecResultOK);
    advance_block();
  }

  // Verify key was deleted
  {
    auto item = lookup_storage_item_from_db(
        make_address_auth_data_addrkey(contract_addr));
    ASSERT_TRUE(item.has_value());
    auto auth_data = item->auth_data();
    ASSERT_FALSE(auth_data.public_keys[0]);
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
