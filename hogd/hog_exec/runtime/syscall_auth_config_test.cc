#include <gtest/gtest.h>

#include <array>
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
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// NOLINTNEXTLINE(misc-use-internal-linkage)
class SyscallAuthConfigTest : public ::testing::Test,
                              public NativeContractTestFramework {
public:
  SyscallAuthConfigTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}
};

// Test WriteAddressRequiredWeight with a valid weight value
TEST_F(SyscallAuthConfigTest, WriteAddressRequiredWeightSuccess)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  // Create a native contract with a method that calls the syscall
  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto status =
            co_await ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(
                Weight{1});

        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
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
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  // Invoke the contract method which calls the syscall
  auto root_res = handler.root_invoke()();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction()();
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
      [public_key](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // 1. Set a public key at index 0
        int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());

        auto pk_status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                0, &key_scheme, key_bytes, &key_weight);

        if (!pk_status.has_value() ||
            pk_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // 2. Set required weight
        auto weight_status =
            co_await ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(
                Weight{3});

        if (!weight_status.has_value() ||
            weight_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        co_return InvokeStatus<void>{};
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
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  // Invoke the contract method which calls both syscalls
  auto root_res = handler.root_invoke()();
  ASSERT_TRUE(root_res.is_root_success());

  // Commit the transaction
  auto commit_res = handler.try_commit_transaction()();
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
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // scheme=null, weight!=null — should be rejected
        uint32_t key_weight = 5;
        auto status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                0, static_cast<const int32_t *>(nullptr),
                std::span<const std::byte>{}, &key_weight);
        if (!status.has_value() ||
            status.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          ADD_FAILURE() << "scheme=null, weight!=null should be rejected";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
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
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto root_res = handler.root_invoke()();
  ASSERT_TRUE(root_res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
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
      [public_key](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        // scheme=null, bytes not empty — should be rejected
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());
        auto status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                0, static_cast<const int32_t *>(nullptr), key_bytes,
                static_cast<const uint32_t *>(nullptr));
        if (!status.has_value() ||
            status.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          ADD_FAILURE() << "scheme=null, bytes not empty should be rejected";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
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
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  auto root_res = handler.root_invoke()();
  ASSERT_TRUE(root_res.is_root_success());
  auto commit_res = handler.try_commit_transaction()();
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
      [public_key](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());
        auto status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                0, &key_scheme, key_bytes, &key_weight);
        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to register key";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
      });
  fns.emplace(
      // Method 1: Delete the key via all-null args
      1, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        auto status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                0, static_cast<const int32_t *>(nullptr),
                std::span<const std::byte>{},
                static_cast<const uint32_t *>(nullptr));
        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to delete key";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
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
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);

    auto root_res = handler.root_invoke()();
    ASSERT_TRUE(root_res.is_root_success());
    auto commit_res = handler.try_commit_transaction()();
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
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        *context);

    auto root_res = handler.root_invoke()();
    ASSERT_TRUE(root_res.is_root_success());
    auto commit_res = handler.try_commit_transaction()();
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

// The remaining reject arms of change_public_key_registration, each
// kSyscallErrorCodeInvalidSyscallInput: scheme non-null with weight null
// (the mixed-null combo the MixedNull tests do not cover), an unknown
// CryptographicScheme, and an Ed25519 key of the wrong size (31 and 33
// bytes -- the size check also guards a fixed-size memcpy). A valid
// registration afterwards proves the rejects left no partial auth state.
TEST_F(SyscallAuthConfigTest, ChangePublicKeyRegistrationRejectsMalformedArgs)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [public_key, _] = ed25519_keygen();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [public_key](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        int32_t ed25519_scheme =
            std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());

        // Not a coroutine: it inspects a status the caller already awaited.
        auto expect_invalid = [&](auto status, const char *what) {
          if (!status.has_value() ||
              status.value() !=
                  SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
            ADD_FAILURE() << what << " must be kInvalidSyscallInput";
            return false;
          }
          return true;
        };

        // scheme != null, weight == null
        if (!expect_invalid(
                co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                    0, &ed25519_scheme, key_bytes,
                    static_cast<const uint32_t *>(nullptr)),
                "scheme non-null with weight null")) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // unknown scheme
        int32_t unknown_scheme = 99;
        if (!expect_invalid(
                co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                    0, &unknown_scheme, key_bytes, &key_weight),
                "unknown scheme")) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // wrong-size Ed25519 keys: one short, one long
        if (!expect_invalid(
                co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                    0, &ed25519_scheme, key_bytes.subspan(0, 31), &key_weight),
                "31-byte Ed25519 key")) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        std::array<std::byte, 33> long_key{};
        if (!expect_invalid(
                co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                    0, &ed25519_scheme, std::span<const std::byte>(long_key),
                    &key_weight),
                "33-byte Ed25519 key")) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        // A valid registration still succeeds after the rejects.
        auto ok = co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
            0, &ed25519_scheme, key_bytes, &key_weight);
        if (!ok.has_value() ||
            ok.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
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
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  advance_block();

  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_TRUE(auth_data.public_keys[0]);
  ASSERT_EQ(auth_data.public_keys[0]->key.ed25519(), public_key);
  ASSERT_EQ(auth_data.public_keys[0]->max_weight, Weight{5});
}

// The key_index gate in change_public_key_registration is strict
// (key_index >= kMaxSigners): index kMaxSigners - 1 registers a key that
// lands in the auth-data row, index kMaxSigners is kInvalidSyscallInput.
// Pins the accept side; the reject side is also pinned through the rv32
// path by SyscallMiscTests.TestChangePubkeyIndexOob.
TEST_F(SyscallAuthConfigTest, ChangePublicKeyRegistrationMaxIndexBoundary)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [public_key, _] = ed25519_keygen();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0,
      [public_key](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());

        auto reject =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                kMaxSigners, &key_scheme, key_bytes, &key_weight);
        if (!reject.has_value() ||
            reject.value() !=
                SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }

        auto accept =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                kMaxSigners - 1, &key_scheme, key_bytes, &key_weight);
        if (!accept.has_value() ||
            accept.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        co_return InvokeStatus<void>{};
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
                                           .calldata = {}},
                            .uniquifier = make_new_uniquifier()}},
      *context);

  ASSERT_TRUE(handler.root_invoke()().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
  advance_block();

  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto auth_data = item->auth_data();
  ASSERT_TRUE(auth_data.public_keys[kMaxSigners - 1]);
  ASSERT_EQ(auth_data.public_keys[kMaxSigners - 1]->key.ed25519(), public_key);
  ASSERT_EQ(auth_data.public_keys[kMaxSigners - 1]->max_weight, Weight{5});
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
