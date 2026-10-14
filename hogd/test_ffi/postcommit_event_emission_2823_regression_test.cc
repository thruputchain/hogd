// Regression test for issue #2823. See test_ffi_test_helpers.h.
//
// A post-commit emit_events() failure used to be mapped onto result_code, so a
// committed transaction was reported as rejected. It is now an FFI error, with
// result_code still ExecResultOK. The failure is forced through a real path: a
// PerTransactionEventServerProxy publishes once per transaction, so consuming
// that slot before the commit makes the wrapper's own emission fail.

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

constexpr ContractAddress kAddress{0x28};
constexpr std::array<std::byte, 4> kPayload{std::byte{0x10}, std::byte{0x20},
                                            std::byte{0x30}, std::byte{0x40}};
constexpr std::string_view kEventPayload = "postcommit_event";

StorageKeyContractData
make_key()
{
  StorageKeyContractData key{};
  key[0] = 0x28;
  key[1] = 0x23;
  return key;
}

void
expect_syscall_ok(ForeignHostFnResult const &result)
{
  EXPECT_EQ(result.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(result.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
}

// Run a root mock frame on `handler` that writes `kPayload` under `key`,
// logs an event, and returns a nonempty buffer, then finish it successfully.
// The event and the return buffer are what post-commit packaging copies.
void
run_root_frame_with_write_and_event(void *handler,
                                    StorageKeyContractData const &key)
{
  Invocation invocation = {
      .invoked_address = kAddress,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler, invocation), kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler), kTestFFIErrorCodeNone);

  expect_syscall_ok(
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kBytestringSet),
                              reinterpret_cast<uint64_t>(&key),
                              reinterpret_cast<uint64_t>(kPayload.data()),
                              kPayload.size(), 0, 0, 0, 0));

  auto *context = SyscallHandlerContext::get_context();
  ASSERT_NE(context, nullptr);
  auto logged = context->to_handler().log_event(
      nullptr, nullptr, nullptr,
      std::as_bytes(std::span(kEventPayload.data(), kEventPayload.size())));
  ASSERT_TRUE(logged.has_value());
  ReturnBuffer return_buffer = {0xaa, 0xbb, 0xcc, 0xdd};
  context->to_handler().set_return_buffer(std::move(return_buffer), false);

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish =
      finish_mock_invocation_frame(handler,
                                   std::to_underlying(InvokeErrorCode::None));
  ASSERT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish.serialized_output);
  free_ffi_data_buffer(finish.serialized_events);
  free_ffi_data_buffer(finish.serialized_trace);
}

// Whether the snapshot holds exactly `kPayload` under {kAddress, key}.
bool
payload_is_committed(void *db, StorageKeyContractData const &key)
{
  StorageKey storage_key(StorageKeyType::ContractData);
  storage_key.contract_data_key() = key;
  auto serialized_addrkey = x23::serialize(
      AddressAndKey{.address = kAddress, .key = std::move(storage_key)});

  auto snapshot = get_snapshot_storage_item(
      db, FFISerializedAddressAndKey{.data = serialized_addrkey.data(),
                                     .len = static_cast<uint32_t>(
                                         serialized_addrkey.size())});
  EXPECT_EQ(snapshot.ffi_error_code, kTestFFIErrorCodeNone);
  if (snapshot.ffi_error_code != kTestFFIErrorCodeNone ||
      snapshot.data.data == nullptr) {
    return false;
  }
  x23::Defer free_snapshot([&] { free_ffi_data_buffer(snapshot.data); });

  WireStorageItem item;
  x23::deserialize(&item, x23::BufView{snapshot.data.data, snapshot.data.len});
  if (item.type() != StorageItemType::kItemTypeBytestring) {
    return false;
  }
  auto const &bytes = item.bytestring();
  return bytes.size() == kPayload.size() &&
         std::memcmp(bytes.data(), kPayload.data(), kPayload.size()) == 0;
}

} // namespace

TEST(TestBindingsTest, TryCommitResultSurvivesPostCommitEmissionFailure)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  ASSERT_EQ(setup_grant_contract_storage_tokens(db, kAddress.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);
  auto *context = static_cast<SyscallHandlerContext *>(handler.syscall_handler);

  auto key = make_key();
  run_root_frame_with_write_and_event(handler.syscall_handler, key);

  // Consume the transaction's one emission ahead of the commit (see the file
  // comment). This drains the frame's events exactly as the wrapper's own
  // emission would have, so what follows exercises the failure path alone.
  auto pre_emit = context->to_handler().emit_events(ExecResult::ExecResultOK,
                                                    ProposedTransaction{})();
  ASSERT_TRUE(pre_emit.has_value());

  auto commit = try_commit_transaction(handler.syscall_handler);
  x23::Defer free_conflict(
      [&] { free_ffi_data_buffer(commit.serialized_conflict); });

  // The harness failure is on the FFI channel and the commit decision is still
  // ExecResultOK. Before the fix this came back with no FFI error and
  // result_code = ExecResultUnrecoverable, with the write committed below.
  EXPECT_EQ(commit.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
  EXPECT_EQ(commit.result_code, std::to_underlying(ExecResult::ExecResultOK));
  EXPECT_EQ(commit.serialized_conflict.data, nullptr);
  EXPECT_TRUE(context->is_committed_or_aborted());

  // The handler closes cleanly (the emission attempt drained the frame's
  // events before it failed), the block advances, and the transaction's write
  // is there.
  ASSERT_EQ(close_syscall_handler(handler.syscall_handler),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
  EXPECT_TRUE(payload_is_committed(db, key));
}

} // namespace hogd
