// Regression test: a StorageDeltaFlagSet with its top bit set, passed to
// SetStorageFlags through the native mock syscall path.
//
// The native SDK lowers a by-value syscall argument with `value as
// SyscallWord`, and off riscv32 a SyscallWord is a u64: the u64 argument slots
// of mock_syscall_entrypoint(). While StorageDeltaFlagSet was a signed int,
// that cast sign-extended any flag set with the top bit set to
// 0xFFFFFFFF'xxxxxxxx. The mock parser fails a value slot that does not fit 32
// bits as an FFI usage error, so the native path answered Unrecoverable where
// RV32, whose slots are 32 bits, delivered the same bits and SetStorageFlags
// returned kSyscallErrorCodeInvalidSyscallInput. StorageDeltaFlagSet is now
// unsigned (groundhogchain/protocol#192), so the cast zero-extends and the
// syscall's own flag check decides on both paths.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

namespace {

// Spelled as bit patterns, so they are the same bits whatever the signedness of
// StorageDeltaFlagSet.
constexpr auto kExclusiveLock = static_cast<StorageDeltaFlagSet>(
    std::to_underlying(StorageDeltaFlags::ExclusiveLock));
constexpr auto kTopBit = static_cast<StorageDeltaFlagSet>(0x8000'0000U);
constexpr auto kAllBits = static_cast<StorageDeltaFlagSet>(0xFFFF'FFFFU);

// The native SDK's lowering of a by-value argument into a u64 slot.
constexpr uint64_t
to_u64_slot(StorageDeltaFlagSet flags)
{
  return static_cast<uint64_t>(flags);
}

// The slot the same lowering produced while StorageDeltaFlagSet was signed.
constexpr uint64_t
to_sign_extended_u64_slot(StorageDeltaFlagSet flags)
{
  return static_cast<uint64_t>(
      static_cast<int64_t>(static_cast<int32_t>(flags)));
}

void
expect_syscall_ok(ForeignHostFnResult const &result)
{
  EXPECT_EQ(result.panic,
            std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
  EXPECT_EQ(result.result_code,
            std::to_underlying(SyscallErrorCode::kSyscallErrorCodeNone));
}

ForeignHostFnResult
bytestring_set(StorageKeyContractData const &key,
               std::span<const std::byte> value)
{
  return mock_syscall_entrypoint(std::to_underlying(SyscallNo::kBytestringSet),
                                 reinterpret_cast<uint64_t>(&key),
                                 reinterpret_cast<uint64_t>(value.data()),
                                 value.size(), 0, 0, 0, 0);
}

ForeignHostFnResult
set_storage_flags(StorageKeyContractData const &key, uint64_t flags_slot)
{
  return mock_syscall_entrypoint(std::to_underlying(
                                     SyscallNo::kSetStorageFlags),
                                 reinterpret_cast<uint64_t>(&key), flags_slot,
                                 0, 0, 0, 0, 0);
}

StorageDeltaFlagSet
get_storage_flags(StorageKeyContractData const &key)
{
  // Seeded with a value the syscall never reports, so a missed write shows.
  StorageDeltaFlagSet flags = kAllBits;
  expect_syscall_ok(
      mock_syscall_entrypoint(std::to_underlying(SyscallNo::kGetStorageFlags),
                              reinterpret_cast<uint64_t>(&key),
                              reinterpret_cast<uint64_t>(&flags), 0, 0, 0, 0,
                              0));
  return flags;
}

// Run `body` in a mock invocation frame whose contract holds storage tokens,
// after writing `key` so a valid flag set has a pending write to attach to.
template<typename Body>
void
run_with_pending_write(StorageKeyContractData const &key, Body &&body)
{
  const ContractAddress contract_addr{0x42};

  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(setup_grant_contract_storage_tokens(db, contract_addr.data(),
                                                1'000'000'000),
            kTestFFIErrorCodeNone);

  auto handler = make_mocked_handler(db, rc);
  ASSERT_EQ(handler.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(handler.syscall_handler, nullptr);
  x23::Defer d3([&] {
    EXPECT_EQ(close_syscall_handler(handler.syscall_handler),
              kTestFFIErrorCodeNone);
  });

  Invocation invocation = {
      .invoked_address = contract_addr,
      .entrypoint = entrypoint_from_method_id(0x1234),
      .gas_limit = 1'000'000,
      .calldata = {},
  };
  ASSERT_EQ(open_frame(handler.syscall_handler, invocation),
            kTestFFIErrorCodeNone);
  ASSERT_EQ(push_context(handler.syscall_handler), kTestFFIErrorCodeNone);

  const std::array<std::byte, 1> value{std::byte{0x01}};
  expect_syscall_ok(bytestring_set(key, value));

  body();

  ASSERT_EQ(pop_context(), kTestFFIErrorCodeNone);
  auto finish =
      finish_mock_invocation_frame(handler.syscall_handler,
                                   std::to_underlying(InvokeErrorCode::None));
  EXPECT_EQ(finish.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(finish.serialized_output);
  free_ffi_data_buffer(finish.serialized_trace);
  free_ffi_data_buffer(finish.serialized_events);
}

} // namespace

// Flag sets with the top bit set reach SetStorageFlags and are rejected by its
// flag check, as on RV32, instead of failing the call as an FFI usage error.
TEST(TestBindingsTest, SetStorageFlagsTopBitIsASyscallErrorOverU64Slot)
{
  const StorageKeyContractData key{};
  run_with_pending_write(key, [&] {
    for (StorageDeltaFlagSet flags :
         {kTopBit, kAllBits, kExclusiveLock | kTopBit}) {
      SCOPED_TRACE(flags);
      auto slot = to_u64_slot(flags);
      EXPECT_LE(slot, std::numeric_limits<uint32_t>::max());

      auto result = set_storage_flags(key, slot);
      EXPECT_EQ(result.panic,
                std::to_underlying(HostFnErrorCode::NoneOrRecoverable));
      EXPECT_EQ(result.result_code,
                std::to_underlying(
                    SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput));
    }

    // A rejected set changes nothing, even one that includes a valid flag.
    EXPECT_EQ(get_storage_flags(key), StorageDeltaFlagSet{0});

    // A valid flag set takes the same path and applies.
    expect_syscall_ok(set_storage_flags(key, to_u64_slot(kExclusiveLock)));
    EXPECT_EQ(get_storage_flags(key), kExclusiveLock);
  });
}

// The slot the signed flag set produced still fails the 32-bit slot check
// before the syscall runs: it is the zero-extension that lets the call through.
TEST(TestBindingsTest, SetStorageFlagsSignExtendedU64SlotIsAnFfiError)
{
  const StorageKeyContractData key{};
  run_with_pending_write(key, [&] {
    for (StorageDeltaFlagSet flags : {kTopBit, kAllBits}) {
      SCOPED_TRACE(flags);
      auto slot = to_sign_extended_u64_slot(flags);
      EXPECT_GT(slot, std::numeric_limits<uint32_t>::max());

      auto result = set_storage_flags(key, slot);
      EXPECT_EQ(result.panic,
                std::to_underlying(HostFnErrorCode::Unrecoverable));
      EXPECT_EQ(result.result_code, 0);
    }

    // The refused calls never reached the syscall, and the frame is still
    // usable.
    EXPECT_EQ(get_storage_flags(key), StorageDeltaFlagSet{0});
    expect_syscall_ok(set_storage_flags(key, to_u64_slot(kExclusiveLock)));
    EXPECT_EQ(get_storage_flags(key), kExclusiveLock);
  });
}

} // namespace hogd
