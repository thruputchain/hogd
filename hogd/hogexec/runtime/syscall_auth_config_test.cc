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
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

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

// Test WriteAddressRequiredWeight with a valid weight value
TEST_F(SyscallAuthConfigTest, WriteAddressRequiredWeightSuccess)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    auto status =
        ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(Weight{1});

    if (!status.has_value() ||
        status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

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

  // Verify the required weight was written
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_EQ(auth_data.required_weight, Weight{1});
}

// Test calling both auth config syscalls: ChangePublicKeyRegistration and
// WriteAddressRequiredWeight
TEST_F(SyscallAuthConfigTest, BothAuthConfigSyscalls)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Generate an Ed25519 key pair
  auto [public_key, _] = ed25519_keygen();

  // Create a native contract with a method that calls both syscalls
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [public_key](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        // 1. Set a public key at index 0
        int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());

        auto pk_status =
            ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(0, &key_scheme,
                                                                 key_bytes,
                                                                 &key_weight);

        if (!pk_status.has_value() ||
            pk_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        // 2. Set required weight
        auto weight_status =
            ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(Weight{3});

        if (!weight_status.has_value() ||
            weight_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          return InvokeError{InvokeErrorCode::Deterministic};
        }

        return {};
      });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

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

  // Invoke the contract method which calls both syscalls
  auto root_res = handler.root_invoke();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction();
  ASSERT_EQ(commit_res, ExecResult::ExecResultOK);

  advance_block();

  // Verify both configs were written
  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();

  // Verify public key was set
  ASSERT_TRUE(auth_data.public_keys[0]);
  ASSERT_EQ(auth_data.public_keys[0]->key.ed25519(), public_key);
  ASSERT_EQ(auth_data.public_keys[0]->max_weight, Weight{5});

  // Verify required weight was set
  ASSERT_EQ(auth_data.required_weight, Weight{3});
}

// Test ChangePublicKeyRegistration rejects mixed-null args:
// scheme=null but weight!=null
TEST_F(SyscallAuthConfigTest, ChangePublicKeyRegistrationMixedNullSchemeNull)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
    // scheme=null, weight!=null — should be rejected
    uint32_t key_weight = 5;
    auto status = ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
        0, static_cast<const int32_t *>(nullptr), std::span<const std::byte>{},
        &key_weight);
    if (!status.has_value() ||
        status.value() !=
            SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
      ADD_FAILURE() << "scheme=null, weight!=null should be rejected";
      return InvokeError{InvokeErrorCode::Deterministic};
    }
    return {};
  });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

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

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [public_key](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
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
          ADD_FAILURE() << "scheme=null, bytes not empty should be rejected";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

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

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      // Method 0: Register a key
      0,
      [public_key](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
        int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());
        auto status =
            ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(0, &key_scheme,
                                                                 key_bytes,
                                                                 &key_weight);
        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to register key";
          return InvokeError{InvokeErrorCode::Deterministic};
        }
        return {};
      });
  fns.emplace(
      // Method 1: Delete the key via all-null args
      1, [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
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
      });
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

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

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
