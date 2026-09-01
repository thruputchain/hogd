// Regression test for issue #2704. See test_ffi_test_helpers.h.
//
// get_snapshot_storage_item() read current_block_id_ and queried the snapshot
// that id named without borrowing the DB, while advance_new_block() changed
// both underneath it. The #3049 guard counted only open handlers, so the read
// was invisible to it and the two raced on plain memory. The read now takes a
// DBContext::Borrow, so the pair is mutually exclusive.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

ContractAddress
make_test_address()
{
  ContractAddress address{};
  address[0] = 0x5a;
  return address;
}

AddressAndKey
make_balance_addrkey(ContractAddress const &address)
{
  return AddressAndKey{
      .address = address,
      .key = StorageKey(StorageKeyType::FreeNativeTokenBalance),
  };
}

void
free_buffer_if_present(FFIDataBuffer buffer)
{
  if (buffer.data != nullptr) {
    EXPECT_EQ(free_ffi_data_buffer(buffer), kTestFFIErrorCodeNone);
  }
}

} // namespace

// Both directions of the exclusion, without depending on thread scheduling, so
// this fails on unfixed code in every build configuration rather than only
// under ThreadSanitizer.
TEST(TestBindingsTest, SnapshotReadAndAdvanceNewBlockExcludeEachOther)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer close_db([db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });
  auto *typed_db = static_cast<DBContext *>(db);

  auto address = make_test_address();
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, address.data(), 12345),
            kTestFFIErrorCodeNone);

  auto serialized_addrkey = x23::serialize(make_balance_addrkey(address));
  FFISerializedAddressAndKey ffi_addrkey{
      .data = serialized_addrkey.data(),
      .len = static_cast<uint32_t>(serialized_addrkey.size()),
  };

  // Uncontended the read succeeds and leaves no borrow behind.
  auto baseline = get_snapshot_storage_item(db, ffi_addrkey);
  EXPECT_EQ(baseline.ffi_error_code, kTestFFIErrorCodeNone);
  free_buffer_if_present(baseline.data);
  EXPECT_FALSE(typed_db->has_open_handlers());

  // A transition in flight refuses the read; before the fix it sailed through.
  {
    DBContext::LifecycleTransition transition(*typed_db);
    ASSERT_TRUE(transition.held());

    auto refused = get_snapshot_storage_item(db, ffi_addrkey);
    EXPECT_EQ(refused.ffi_error_code, kTestFFIErrorCodeUnrecoverableError);
    EXPECT_EQ(refused.data.data, nullptr);
  }

  // And an in-flight read refuses the advance, which is the half that keeps
  // advance_new_block() off current_block_id_ mid-read.
  {
    DBContext::Borrow borrow(*typed_db);
    ASSERT_TRUE(borrow.held());
    EXPECT_TRUE(typed_db->has_open_handlers());

    EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeUnrecoverableError);
  }

  EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
  auto after_advance = get_snapshot_storage_item(db, ffi_addrkey);
  EXPECT_EQ(after_advance.ffi_error_code, kTestFFIErrorCodeNone);
  free_buffer_if_present(after_advance.data);
  EXPECT_FALSE(typed_db->is_lifecycle_poisoned());
}

// The reported reproduction: concurrent honest callers on one DB handle. Under
// ThreadSanitizer this is what reports the current_block_id_ read/write pair on
// an unfixed build.
TEST(TestBindingsTest, SnapshotStorageItemReadRacesAdvanceNewBlock)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer close_db([db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });
  auto *typed_db = static_cast<DBContext *>(db);

  auto address = make_test_address();
  ASSERT_EQ(setup_grant_contract_storage_tokens(db, address.data(), 12345),
            kTestFFIErrorCodeNone);

  auto serialized_addrkey = x23::serialize(make_balance_addrkey(address));
  FFISerializedAddressAndKey ffi_addrkey{
      .data = serialized_addrkey.data(),
      .len = static_cast<uint32_t>(serialized_addrkey.size()),
  };

  auto warmup = get_snapshot_storage_item(db, ffi_addrkey);
  ASSERT_EQ(warmup.ffi_error_code, kTestFFIErrorCodeNone);
  free_buffer_if_present(warmup.data);

  constexpr int kReaderThreads = 2;
  constexpr int kAdvanceIterations = 16;

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> advancer_done{false};

  std::atomic<int> read_attempts{0};
  std::atomic<int> successful_reads{0};
  std::atomic<int> refused_reads{0};
  std::atomic<int> unexpected_read_errors{0};

  auto reader = [&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    while (!advancer_done.load(std::memory_order_acquire)) {
      auto result = get_snapshot_storage_item(db, ffi_addrkey);
      read_attempts.fetch_add(1, std::memory_order_relaxed);
      if (result.ffi_error_code == kTestFFIErrorCodeNone) {
        successful_reads.fetch_add(1, std::memory_order_relaxed);
        free_buffer_if_present(result.data);
      }
      else if (result.ffi_error_code == kTestFFIErrorCodeUnrecoverableError) {
        // The documented refusal: an advance was in flight, nothing to free.
        refused_reads.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(result.data.data, nullptr);
      }
      else {
        unexpected_read_errors.fetch_add(1, std::memory_order_relaxed);
        free_buffer_if_present(result.data);
      }

      // Back off so the advancer can find a gap between our borrows. yield()
      // is not enough: on a machine with cores to spare it returns straight to
      // this loop, and two readers then keep a borrow outstanding essentially
      // always, starving the advance.
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  };

  std::vector<std::thread> readers;
  readers.reserve(kReaderThreads);
  for (int i = 0; i < kReaderThreads; i++) {
    readers.emplace_back(reader);
  }

  std::thread advancer([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (int i = 0; i < kAdvanceIterations; i++) {
      // A refusal here is the documented outcome when a reader holds a borrow,
      // and nothing below asserts how many advances land, so there is no retry
      // to get wrong: advance_new_block() reports contention and a permanently
      // poisoned DB with the same code, and retrying the latter would spin and
      // then surface as a confusing advance-count failure instead of the real
      // one. Stop on poison; the assertions after the join report it.
      if (advance_new_block(db, nullptr) != kTestFFIErrorCodeNone &&
          typed_db->is_lifecycle_poisoned()) {
        break;
      }
      // Leave a clear window for the readers between advances.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    advancer_done.store(true, std::memory_order_release);
  });

  while (ready.load(std::memory_order_acquire) < kReaderThreads + 1) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  advancer.join();
  for (auto &thread : readers) {
    thread.join();
  }

  // Under contention neither side is guaranteed to win: an advance holds the
  // exclusive transition for the length of a snapshot close/commit/create,
  // while a read holds its borrow for one RPC. So assert what does hold no
  // matter who wins -- every read either succeeded or was refused, nothing
  // else -- and leave the exclusion itself to the deterministic test above.
  EXPECT_GT(read_attempts.load(), 0);
  EXPECT_EQ(unexpected_read_errors.load(), 0);
  EXPECT_EQ(successful_reads.load() + refused_reads.load(),
            read_attempts.load());
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_FALSE(typed_db->is_lifecycle_poisoned());

  // Both paths still work once the contention is over.
  auto final_read = get_snapshot_storage_item(db, ffi_addrkey);
  EXPECT_EQ(final_read.ffi_error_code, kTestFFIErrorCodeNone);
  free_buffer_if_present(final_read.data);
  EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
}

} // namespace hogd
