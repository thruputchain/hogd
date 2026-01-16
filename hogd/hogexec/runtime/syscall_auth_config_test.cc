#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "account.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/syscall_inner.h"
#include "hogd/rust/contract_types/bindings.h"
#include "rpc_types.hh"
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
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             generated::NonceConfig ffi_nonce_config{};
             ffi_nonce_config.accepted_keys = 0x01; // Accept key 0
             ffi_nonce_config.required_weight = Weight{1};

             auto [status, gas] =
                 syscall_WriteAddressNonceConfig_inner(handler,
                                                       ffi_nonce_config);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
      make_account_auth_data_addrkey(contract_addr));
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
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
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
                     handler, &ffi_config0, &ffi_config1, nullptr, nullptr,
                     nullptr);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
      make_account_auth_data_addrkey(contract_addr));
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
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // All configs are null - should succeed with empty list
             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     handler, nullptr, nullptr, nullptr, nullptr, nullptr);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
      make_account_auth_data_addrkey(contract_addr));
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
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
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
                     handler, &ffi_config0, nullptr, &ffi_config2, nullptr,
                     nullptr);

             if (!status.has_value() ||
                 status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             generated::GasPermissionConfig ffi_config0{};
             ffi_config0.limit = GasAmount{1000};
             ffi_config0.accepted_keys = 0x01;
             ffi_config0.required_weight = Weight{1};

             // Configs followed by nulls should be fine
             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     handler, &ffi_config0, nullptr, nullptr, nullptr, nullptr);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
          {0,
           [](SystemCallHandler &handler,
              NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             generated::GasPermissionConfig ffi_configs[5];
             for (int i = 0; i < 5; i++) {
               ffi_configs[i].limit =
                   GasAmount{static_cast<uint64_t>(1000 + (i * 100))};
               ffi_configs[i].accepted_keys = BitMap{1ULL << i};
               ffi_configs[i].required_weight =
                   Weight{static_cast<uint32_t>(i + 1)};
             }

             auto [status, gas] =
                 syscall_WriteAddressGasPermissionConfigs_inner(
                     handler, &ffi_configs[0], &ffi_configs[1], &ffi_configs[2],
                     &ffi_configs[3], &ffi_configs[4]);

             if (!status.has_value() ||
                 status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
      make_account_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_EQ(auth_data.gas_permissions.size(), 5);
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(auth_data.gas_permissions[i].limit,
              GasAmount{static_cast<uint64_t>(1000 + (i * 100))});
    ASSERT_EQ(auth_data.gas_permissions[i].accepted_keys, BitMap{1ULL << i});
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
          {0,
           [public_key](SystemCallHandler &handler,
                        NativeRuntime & /*unused*/) -> InvokeStatus<void> {
             // 1. Set a public key at index 0
             int32_t key_scheme =
                 std::to_underlying(CryptographicScheme::Ed25519);
             uint32_t key_weight = 5;
             std::span<const std::byte> key_bytes(
                 reinterpret_cast<const std::byte *>(public_key.data()),
                 public_key.size());

             auto [pk_status, pk_gas] =
                 syscall_ChangePublicKeyRegistration_inner(handler, 0,
                                                           &key_scheme,
                                                           key_bytes,
                                                           &key_weight);

             if (!pk_status.has_value() ||
                 pk_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
             }

             // 2. Set nonce config
             generated::NonceConfig ffi_nonce_config{};
             ffi_nonce_config.accepted_keys = 0x01; // Accept key 0
             ffi_nonce_config.required_weight = Weight{3};

             auto [nonce_status, nonce_gas] =
                 syscall_WriteAddressNonceConfig_inner(handler,
                                                       ffi_nonce_config);

             if (!nonce_status.has_value() ||
                 nonce_status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
                     handler, &ffi_config0, &ffi_config1, nullptr, nullptr,
                     nullptr);

             if (!gas_status.has_value() ||
                 gas_status.value() !=
                     SyscallErrorCode::kSyscallErrorCodeNone) {
               return InvokeError{InvokeErrorCode::kInvokeDeterministic};
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
      make_account_auth_data_addrkey(contract_addr));
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

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
