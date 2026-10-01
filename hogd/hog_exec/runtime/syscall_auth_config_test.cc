#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/item/item_view.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "rpc_types.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/x23.h"

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

  // Creates native contract 0 with method table [fns] and deploys it at
  // [contract_addr]. The returned context is what every handler running it
  // needs.
  std::unique_ptr<NativeContext> deploy_native_contract(
      ContractAddress const &contract_addr,
      std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns)
  {
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::make_unique<NativeContract>(std::move(fns)));
    auto context = std::make_unique<NativeContext>(std::move(contract_map));

    auto contract = make_native_contract(0);
    write_contract(contract);
    write_deployment(contract_addr, contract);
    return context;
  }

  // Runs method [method_id] of the contract at [contract_addr] as one
  // transaction, commits it, and advances the block. Fails the test on any
  // step; wrap calls in ASSERT_NO_FATAL_FAILURE.
  void run_and_commit(ContractAddress const &contract_addr,
                      NativeContext &context, Runtime::MethodID method_id)
  {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx = Transaction{.invocation =
                                  Invocation{.invoked_address = contract_addr,
                                             .entrypoint =
                                                 entrypoint_from_method_id(
                                                     method_id),
                                             .gas_limit = UINT64_MAX,
                                             .calldata = {}},
                              .uniquifier = make_new_uniquifier()}},
        context);
    ASSERT_TRUE(handler.root_invoke()().is_root_success());
    ASSERT_EQ(handler.try_commit_transaction()(), ExecResult::ExecResultOK);
    advance_block();
  }
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

// A key-only first registration commits kDefaultNewAddressRequiredWeight
// rather than zero (issue #3069).
TEST_F(SyscallAuthConfigTest,
       KeyOnlyFirstRegistrationTakesDefaultRequiredWeight)
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
  auto native_contract = std::make_unique<NativeContract>(std::move(fns));

  std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
  contract_map.emplace(0, std::move(native_contract));
  auto context = std::make_unique<NativeContext>(std::move(contract_map));

  auto contract = make_native_contract(0);
  write_contract(contract);
  write_deployment(contract_addr, contract);

  ASSERT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());

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
  EXPECT_EQ(auth_data.public_keys[0]->key.ed25519(), public_key);
  EXPECT_EQ(auth_data.public_keys[0]->max_weight, Weight{5});
  EXPECT_EQ(auth_data.required_weight,
            Weight{kDefaultNewAddressRequiredWeight});
}

// Removing a key from an address with no auth row writes nothing, so no row is
// created. A present row, even an empty one, would take the address off the
// kPubkeySignature path (issue #3069).
TEST_F(SyscallAuthConfigTest, DeletionOnlyFirstTouchCreatesNoRow)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
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

  ASSERT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());

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

  EXPECT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());
}

// A key removal on an address with no auth row writes nothing, but it still
// takes the address's auth lock like any other auth change, so a second auth
// change to that address in the same block conflicts.
TEST_F(SyscallAuthConfigTest, DeletionOnlyFirstTouchTakesAuthLock)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [public_key, _] = ed25519_keygen();

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(
      // Method 0: remove the key at slot 0
      0, [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
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
  fns.emplace(
      // Method 1: register a key at slot 1
      1,
      [public_key](
          NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
        int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
        uint32_t key_weight = 5;
        std::span<const std::byte> key_bytes(
            reinterpret_cast<const std::byte *>(public_key.data()),
            public_key.size());
        auto status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                1, &key_scheme, key_bytes, &key_weight);
        if (!status.has_value() ||
            status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to register key";
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

  ASSERT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());

  auto make_tx = [&](Runtime::MethodID method) {
    return SignedTransaction{
        .tx = Transaction{.invocation =
                              Invocation{.invoked_address = contract_addr,
                                         .entrypoint =
                                             entrypoint_from_method_id(method),
                                         .gas_limit = UINT64_MAX,
                                         .calldata = {}},
                          .uniquifier = make_new_uniquifier()}};
  };

  auto removal = make_new_system_call_handler(make_tx(0), *context);
  ASSERT_TRUE(removal.root_invoke()().is_root_success());
  ASSERT_EQ(removal.try_commit_transaction()(), ExecResult::ExecResultOK);

  // Same block: the registration conflicts on the lock the removal holds.
  auto registration = make_new_system_call_handler(make_tx(1), *context);
  ASSERT_TRUE(registration.root_invoke()().is_root_success());
  ASSERT_EQ(registration.try_commit_transaction()(),
            ExecResult::ExecResultTxNonDeterministicError);
  // Proposal mode: hog_db selects All in debug builds, First in release
  // builds.
  if constexpr (is_debug_build()) {
    bool found_conflict = false;
    for (auto const &c : registration.last_commit_conflict().conflicts()) {
      if (c.code == DbConflictCode::ExclusiveLockDenied) {
        found_conflict = true;
      }
    }
    EXPECT_TRUE(found_conflict);
  }
  else {
    EXPECT_EQ(registration.last_commit_conflict().first().code,
              DbConflictCode::ExclusiveLockDenied);
  }

  advance_block();

  // The removal wrote no row, and the registration never committed.
  EXPECT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());

  // With the lock released, the same registration commits in the next block.
  auto retry = make_new_system_call_handler(make_tx(1), *context);
  ASSERT_TRUE(retry.root_invoke()().is_root_success());
  ASSERT_EQ(retry.try_commit_transaction()(), ExecResult::ExecResultOK);
  advance_block();

  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  EXPECT_TRUE(item->auth_data().public_keys[1]);
}

// Writing a zero threshold with the first key is how an address opts into
// being publicly originatable; the default does not override it.
TEST_F(SyscallAuthConfigTest, ExplicitZeroRequiredWeightOverridesDefault)
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
        auto pk_status =
            co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
                0, &key_scheme, key_bytes, &key_weight);
        if (!pk_status.has_value() ||
            pk_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to register key";
          co_return InvokeError{InvokeErrorCode::Deterministic};
        }
        auto weight_status =
            co_await ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(
                Weight{0});
        if (!weight_status.has_value() ||
            weight_status.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
          ADD_FAILURE() << "Failed to write required weight";
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
  EXPECT_EQ(auth_data.public_keys[0]->key.ed25519(), public_key);
  EXPECT_EQ(auth_data.required_weight, Weight{0});
}

// === GetAddressAuthData ===

namespace {

constexpr SyscallErrorCode kNone = SyscallErrorCode::kSyscallErrorCodeNone;

// An empty registration encodes as kMaxSigners absent optionals, the weight
// and the height; a full one adds a PublicKeyConfig (36-byte key union plus
// weight) per slot. The syscall description quotes the latter as the bound.
constexpr RuntimeMemoryLength kEmptyRegistrationXdrSize =
    (kMaxSigners * 4) + 4 + 8;
constexpr RuntimeMemoryLength kFullRegistrationXdrSize =
    (kMaxSigners * (4 + 36 + 4)) + 4 + 8;
static_assert(kEmptyRegistrationXdrSize == 96);
static_assert(kFullRegistrationXdrSize == 936);

// Comfortably holds any encoding.
constexpr size_t kAuthDataBufferSize = 1024;

PublicKeyConfigOpt
make_key_config(auto const &ed25519_public_key, Weight max_weight)
{
  PublicKey key;
  key.scheme(CryptographicScheme::Ed25519);
  key.ed25519() = ed25519_public_key;
  return PublicKeyConfigOpt{x23::Ptr<PublicKeyConfig>{
      new PublicKeyConfig{.key = key, .max_weight = max_weight}}};
}

AddressAuthData
decode_auth_data(std::span<const std::byte> bytes)
{
  AddressAuthData out{};
  x23::deserialize(&out, x23::BufView{bytes});
  return out;
}

// Reads the current address's registration through the syscall into a
// buffer that holds any encoding. Fails the test and returns nullopt on any
// error, so callers can bail out of the contract body.
x23::Async<std::optional<AddressAuthData>>
read_auth_data(NativeSyscallContext &ctx)
{
  std::array<std::byte, kAuthDataBufferSize> buffer{};
  RuntimeMemoryLength written_len = 0;
  auto status = co_await ctx.syscall<SyscallNo::kGetAddressAuthData>(
      std::span<std::byte>{buffer}, written_len);
  if (!status.has_value() || status.value() != kNone) {
    ADD_FAILURE() << "GetAddressAuthData failed";
    co_return std::nullopt;
  }
  if (written_len > buffer.size()) {
    ADD_FAILURE() << "GetAddressAuthData reported " << written_len
                  << " bytes, more than any encoding";
    co_return std::nullopt;
  }
  co_return decode_auth_data(
      std::span<const std::byte>{buffer.data(), written_len});
}

bool
is_empty_registration(AddressAuthData const &auth_data)
{
  for (size_t i = 0; i < kMaxSigners; ++i) {
    if (auth_data.public_keys[i]) {
      return false;
    }
  }
  return auth_data.required_weight == 0 && auth_data.last_written_height == 0;
}

void
expect_slot(AddressAuthData const &auth_data, size_t index,
            auto const &ed25519_public_key, Weight max_weight)
{
  if (!auth_data.public_keys[index]) {
    ADD_FAILURE() << "slot " << index << " is empty";
    return;
  }
  EXPECT_EQ(auth_data.public_keys[index]->key.scheme(),
            CryptographicScheme::Ed25519);
  EXPECT_EQ(auth_data.public_keys[index]->key.ed25519(), ed25519_public_key);
  EXPECT_EQ(auth_data.public_keys[index]->max_weight, max_weight);
}

void
expect_slots_empty_except(AddressAuthData const &auth_data,
                          std::initializer_list<size_t> occupied)
{
  for (size_t i = 0; i < kMaxSigners; ++i) {
    const bool expected = std::ranges::find(occupied, i) != occupied.end();
    EXPECT_EQ(static_cast<bool>(auth_data.public_keys[i]), expected)
        << "slot " << i;
  }
}

// The register-a-key syscall body every test below shares.
x23::Async<bool>
register_key(NativeSyscallContext &ctx, uint32_t index,
             auto const &ed25519_public_key, Weight max_weight)
{
  int32_t key_scheme = std::to_underlying(CryptographicScheme::Ed25519);
  uint32_t key_weight = max_weight;
  std::span<const std::byte> key_bytes(reinterpret_cast<const std::byte *>(
                                           ed25519_public_key.data()),
                                       ed25519_public_key.size());
  auto status = co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
      index, &key_scheme, key_bytes, &key_weight);
  if (!status.has_value() || status.value() != kNone) {
    ADD_FAILURE() << "failed to register slot " << index;
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
remove_key(NativeSyscallContext &ctx, uint32_t index)
{
  auto status = co_await ctx.syscall<SyscallNo::kChangePublicKeyRegistration>(
      index, static_cast<const int32_t *>(nullptr),
      std::span<const std::byte>{}, static_cast<const uint32_t *>(nullptr));
  if (!status.has_value() || status.value() != kNone) {
    ADD_FAILURE() << "failed to remove slot " << index;
    co_return false;
  }
  co_return true;
}

x23::Async<bool>
set_required_weight(NativeSyscallContext &ctx, Weight required_weight)
{
  auto status = co_await ctx.syscall<SyscallNo::kWriteAddressRequiredWeight>(
      required_weight);
  if (!status.has_value() || status.value() != kNone) {
    ADD_FAILURE() << "failed to set required weight";
    co_return false;
  }
  co_return true;
}

} // namespace

// The encoding bounds the syscall description quotes.
TEST(AddressAuthDataXdrSizeTest, EncodingBoundsMatchSyscallDescription)
{
  AddressAuthData empty{};
  EXPECT_EQ(x23::xdr_size(empty), kEmptyRegistrationXdrSize);

  auto [public_key, secret_key] = ed25519_keygen();
  AddressAuthData full{};
  for (size_t i = 0; i < kMaxSigners; ++i) {
    full.public_keys[i] = make_key_config(public_key, 1);
  }
  EXPECT_EQ(x23::xdr_size(full), kFullRegistrationXdrSize);
}

// compose() over a committed row: changed slots replace the base's, a null
// change clears one, untouched slots and the weight carry over, and the row
// is stamped with the executing height. The base itself is left alone.
TEST(AddressAuthDataItemTest, ComposeAppliesDeltaOverBase)
{
  auto [key0, secret0] = ed25519_keygen();
  auto [key1, secret1] = ed25519_keygen();

  AddressAuthData base{};
  base.public_keys[0] = make_key_config(key0, 5);
  base.public_keys[2] = make_key_config(key1, 4);
  base.required_weight = 3;
  base.last_written_height = 10;

  AddressAuthDataItem item;
  item.change_public_key(1, make_key_config(key1, 1));
  item.change_public_key(0, PublicKeyConfigOpt{nullptr});
  EXPECT_TRUE(item.writes_row(true));

  auto composed = item.compose(&base, 42);
  expect_slots_empty_except(composed, {1, 2});
  expect_slot(composed, 1, key1, 1);
  expect_slot(composed, 2, key1, 4);
  EXPECT_EQ(composed.required_weight, 3U);
  EXPECT_EQ(composed.last_written_height, 42U);

  expect_slot(base, 0, key0, 5);
  EXPECT_EQ(base.last_written_height, 10U);

  item.set_required_weight(9);
  auto reweighted = item.compose(&base, 43);
  EXPECT_EQ(reweighted.required_weight, 9U);
  EXPECT_EQ(reweighted.last_written_height, 43U);

  // No committed row: the delta lands on an empty registration.
  auto fresh = item.compose(nullptr, 44);
  expect_slots_empty_except(fresh, {1});
  expect_slot(fresh, 1, key1, 1);
  EXPECT_EQ(fresh.required_weight, 9U);
  EXPECT_EQ(fresh.last_written_height, 44U);
}

// A delta that only clears slots of an address with no row writes nothing in
// hog_db (AuthLock::writes_row), so compose() reports the row as it is:
// absent, unstamped. Over an existing row the same delta is a write.
TEST(AddressAuthDataItemTest, ClearOnlyDeltaWritesNoRow)
{
  AddressAuthDataItem item;
  item.change_public_key(0, PublicKeyConfigOpt{nullptr});
  EXPECT_FALSE(item.writes_row(false));
  EXPECT_TRUE(item.writes_row(true));

  EXPECT_TRUE(is_empty_registration(item.compose(nullptr, 7)));

  AddressAuthData base{};
  base.last_written_height = 3;
  EXPECT_EQ(item.compose(&base, 7).last_written_height, 7U);

  item.set_required_weight(0);
  EXPECT_TRUE(item.writes_row(false));
  EXPECT_EQ(item.compose(nullptr, 7).last_written_height, 7U);
}

// An address with no auth row reads as an empty registration, not an error.
// A short buffer still reports the full length and receives the prefix that
// fits; an empty buffer only measures. Reading writes nothing.
TEST_F(SyscallAuthConfigTest, GetAddressAuthDataFreshAddressReadsEmpty)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto auth_data = co_await read_auth_data(ctx);
                if (!auth_data) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                EXPECT_TRUE(is_empty_registration(*auth_data));
                auto encoded = x23::serialize(*auth_data);
                EXPECT_EQ(encoded.size(), kEmptyRegistrationXdrSize);

                std::array<std::byte, 8> short_buffer{};
                RuntimeMemoryLength written_len = 0;
                auto short_status =
                    co_await ctx.syscall<SyscallNo::kGetAddressAuthData>(
                        std::span<std::byte>{short_buffer}, written_len);
                EXPECT_TRUE(short_status.has_value());
                EXPECT_EQ(short_status.value(), kNone);
                EXPECT_EQ(written_len, kEmptyRegistrationXdrSize);
                EXPECT_TRUE(std::equal(short_buffer.begin(), short_buffer.end(),
                                       reinterpret_cast<const std::byte *>(
                                           encoded.data())));

                RuntimeMemoryLength measured_len = 0;
                auto measure_status =
                    co_await ctx.syscall<SyscallNo::kGetAddressAuthData>(
                        std::span<std::byte>{}, measured_len);
                EXPECT_TRUE(measure_status.has_value());
                EXPECT_EQ(measure_status.value(), kNone);
                EXPECT_EQ(measured_len, kEmptyRegistrationXdrSize);
                co_return InvokeStatus<void>{};
              });
  auto context = deploy_native_contract(contract_addr, std::move(fns));

  ASSERT_NO_FATAL_FAILURE(run_and_commit(contract_addr, *context, 0));
  EXPECT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());
}

// The acceptance scenario: a registration made earlier in the same frame is
// visible to the read, stamped with the executing height; after the block
// commits a later transaction reads the same row back from the snapshot; a
// removal in that transaction clears the slot while the committed weight
// stays.
TEST_F(SyscallAuthConfigTest, GetAddressAuthDataSeesPendingAndCommittedWrites)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [public_key, secret_key] = ed25519_keygen();
  BlockHeight registration_height = 0;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  // Method 0: register slot 3 at weight 2 and require weight 2, then read it
  // back in the same frame.
  fns.emplace(0,
              [public_key, &registration_height](
                  NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                if (!co_await register_key(ctx, 3, public_key, 2) ||
                    !co_await set_required_weight(ctx, 2)) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                auto auth_data = co_await read_auth_data(ctx);
                if (!auth_data) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                expect_slot(*auth_data, 3, public_key, 2);
                expect_slots_empty_except(*auth_data, {3});
                EXPECT_EQ(auth_data->required_weight, 2U);
                EXPECT_EQ(auth_data->last_written_height,
                          ctx.handler().get_block_height());
                registration_height = ctx.handler().get_block_height();
                co_return InvokeStatus<void>{};
              });
  // Method 1, a later block: the committed row reads back unchanged; removing
  // the slot is visible at once, with the committed weight intact.
  fns.emplace(1,
              [public_key, &registration_height](
                  NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto committed = co_await read_auth_data(ctx);
                if (!committed) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                expect_slot(*committed, 3, public_key, 2);
                expect_slots_empty_except(*committed, {3});
                EXPECT_EQ(committed->required_weight, 2U);
                EXPECT_EQ(committed->last_written_height, registration_height);
                EXPECT_LT(registration_height,
                          ctx.handler().get_block_height());

                if (!co_await remove_key(ctx, 3)) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                auto cleared = co_await read_auth_data(ctx);
                if (!cleared) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                expect_slots_empty_except(*cleared, {});
                EXPECT_EQ(cleared->required_weight, 2U);
                EXPECT_EQ(cleared->last_written_height,
                          ctx.handler().get_block_height());
                co_return InvokeStatus<void>{};
              });
  auto context = deploy_native_contract(contract_addr, std::move(fns));

  ASSERT_NO_FATAL_FAILURE(run_and_commit(contract_addr, *context, 0));
  {
    auto item = lookup_storage_item_from_db(
        make_address_auth_data_addrkey(contract_addr));
    ASSERT_TRUE(item.has_value());
    auto const &row = item->auth_data();
    expect_slot(row, 3, public_key, 2);
    expect_slots_empty_except(row, {3});
    EXPECT_EQ(row.required_weight, 2U);
    EXPECT_EQ(row.last_written_height, registration_height);
  }

  ASSERT_NO_FATAL_FAILURE(run_and_commit(contract_addr, *context, 1));
  {
    auto item = lookup_storage_item_from_db(
        make_address_auth_data_addrkey(contract_addr));
    ASSERT_TRUE(item.has_value());
    auto const &row = item->auth_data();
    expect_slots_empty_except(row, {});
    EXPECT_EQ(row.required_weight, 2U);
  }
}

// Base-plus-delta through the syscall: an untouched read is the committed
// row; a write to another slot layers over it without disturbing the
// committed slot; a weight write replaces the committed weight.
TEST_F(SyscallAuthConfigTest,
       GetAddressAuthDataComposesPendingDeltaOverCommittedRow)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto [key0, secret0] = ed25519_keygen();
  auto [key1, secret1] = ed25519_keygen();
  BlockHeight first_height = 0;

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [key0, &first_height](
                  NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                if (!co_await register_key(ctx, 0, key0, 5) ||
                    !co_await set_required_weight(ctx, 3)) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                first_height = ctx.handler().get_block_height();
                co_return InvokeStatus<void>{};
              });
  fns.emplace(1,
              [key0, key1, &first_height](
                  NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                auto before = co_await read_auth_data(ctx);
                if (!before) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                expect_slot(*before, 0, key0, 5);
                expect_slots_empty_except(*before, {0});
                EXPECT_EQ(before->required_weight, 3U);
                EXPECT_EQ(before->last_written_height, first_height);

                if (!co_await register_key(ctx, 1, key1, 1)) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                auto after_key = co_await read_auth_data(ctx);
                if (!after_key) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                expect_slot(*after_key, 0, key0, 5);
                expect_slot(*after_key, 1, key1, 1);
                expect_slots_empty_except(*after_key, {0, 1});
                EXPECT_EQ(after_key->required_weight, 3U);
                EXPECT_EQ(after_key->last_written_height,
                          ctx.handler().get_block_height());

                if (!co_await set_required_weight(ctx, 7)) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                auto after_weight = co_await read_auth_data(ctx);
                if (!after_weight) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                expect_slot(*after_weight, 0, key0, 5);
                expect_slot(*after_weight, 1, key1, 1);
                EXPECT_EQ(after_weight->required_weight, 7U);
                co_return InvokeStatus<void>{};
              });
  auto context = deploy_native_contract(contract_addr, std::move(fns));

  ASSERT_NO_FATAL_FAILURE(run_and_commit(contract_addr, *context, 0));
  ASSERT_NO_FATAL_FAILURE(run_and_commit(contract_addr, *context, 1));

  auto item = lookup_storage_item_from_db(
      make_address_auth_data_addrkey(contract_addr));
  ASSERT_TRUE(item.has_value());
  auto const &row = item->auth_data();
  expect_slot(row, 0, key0, 5);
  expect_slot(row, 1, key1, 1);
  expect_slots_empty_except(row, {0, 1});
  EXPECT_EQ(row.required_weight, 7U);
}

// Clearing a slot of an address with no row writes nothing (see
// DeletionOnlyFirstTouchCreatesNoRow), and the read agrees: still empty,
// still unstamped.
TEST_F(SyscallAuthConfigTest, GetAddressAuthDataClearOnlyDeltaReadsEmpty)
{
  auto contract_addr = make_contract_address(0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
  fns.emplace(0,
              [](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
                if (!co_await remove_key(ctx, 0)) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                auto auth_data = co_await read_auth_data(ctx);
                if (!auth_data) {
                  co_return InvokeError{InvokeErrorCode::Deterministic};
                }
                EXPECT_TRUE(is_empty_registration(*auth_data));
                co_return InvokeStatus<void>{};
              });
  auto context = deploy_native_contract(contract_addr, std::move(fns));

  ASSERT_NO_FATAL_FAILURE(run_and_commit(contract_addr, *context, 0));
  EXPECT_FALSE(
      lookup_storage_item_from_db(make_address_auth_data_addrkey(contract_addr))
          .has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
