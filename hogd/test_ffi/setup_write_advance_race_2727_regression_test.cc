// Regression test for issue #2727. See test_ffi_test_helpers.h.
//
// The setup_* writers put() to the base DB and advance_new_block() sync()s it
// to commit, which RAMDB forbids overlapping. The writers now borrow the DB
// across the write, so a writer and an advance refuse each other.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/db_context_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

constexpr int32_t kMockContractId = 27;

ContractAddress
make_contract_address(uint8_t tag)
{
  ContractAddress address{};
  address[0] = tag;
  return address;
}

// The mock contract the create/deploy writers store. Registering the id with
// the runtime context is what lets setup_create_contract() admit it.
Contract
make_registered_contract(void *rc)
{
  EXPECT_EQ(register_new_contract(rc, kMockContractId, mock_contract_return,
                                  nullptr),
            kTestFFIErrorCodeNone);
  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = kMockContractId;
  return contract;
}

AddressAndKey
make_balance_addrkey(ContractAddress const &address)
{
  return AddressAndKey{
      .address = address,
      .key = StorageKey(StorageKeyType::FreeNativeTokenBalance),
  };
}

// Whether the snapshot holds a balance item for `address`.
bool
has_balance_item(void *db, ContractAddress const &address)
{
  auto serialized_addrkey = x23::serialize(make_balance_addrkey(address));
  auto result = get_snapshot_storage_item(
      db, FFISerializedAddressAndKey{.data = serialized_addrkey.data(),
                                     .len = static_cast<uint32_t>(
                                         serialized_addrkey.size())});
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  if (result.data.data == nullptr) {
    return false;
  }
  EXPECT_EQ(free_ffi_data_buffer(result.data), kTestFFIErrorCodeNone);
  return true;
}

} // namespace

// Both directions of the exclusion, without depending on thread scheduling, so
// this fails on unfixed code in every build configuration rather than only
// under ThreadSanitizer.
TEST(TestBindingsTest, SetupWritersAndAdvanceNewBlockExcludeEachOther)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });
  auto *typed_db = static_cast<DBContext *>(db);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  auto contract = make_registered_contract(rc);
  auto contract_bytes = x23::serialize(contract);
  auto contract_hash = sha256(contract);
  auto address = make_contract_address(0x27);

  // With a lifecycle transition in flight every writer is refused. Before the
  // fix all three sailed through into the base DB.
  {
    DBContext::LifecycleTransition transition(*typed_db);
    ASSERT_TRUE(transition.held());

    EXPECT_EQ(setup_grant_contract_storage_tokens(db, address.data(), 1000),
              kTestFFIErrorCodeUnrecoverableError);
    EXPECT_EQ(setup_create_contract(rc, db, contract_bytes.data(),
                                    contract_bytes.size()),
              kTestFFIErrorCodeUnrecoverableError);
    EXPECT_EQ(setup_deploy_contract(db, address.data(), contract_hash.data(),
                                    contract_bytes.size()),
              kTestFFIErrorCodeUnrecoverableError);
    // A refused writer leaves no borrow behind.
    EXPECT_FALSE(typed_db->has_open_handlers());
  }

  // A refused writer wrote nothing: the grant left no balance, and the create
  // left no contract row for a deploy to find.
  EXPECT_FALSE(has_balance_item(db, address));
  EXPECT_EQ(setup_deploy_contract(db, address.data(), contract_hash.data(),
                                  contract_bytes.size()),
            kTestFFIErrorCodeDeployNonexistentContract);

  // In the other direction a writer mid-put() refuses the advance, which is
  // the half that keeps the commit's sync() off a write in progress.
  {
    DBContext::Borrow borrow(*typed_db);
    ASSERT_TRUE(borrow.held());
    EXPECT_EQ(advance_new_block(db, nullptr),
              kTestFFIErrorCodeUnrecoverableError);
  }

  // Uncontended, all three land, leave no borrow, and the advance works again.
  EXPECT_EQ(setup_grant_contract_storage_tokens(db, address.data(), 1000),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_create_contract(rc, db, contract_bytes.data(),
                                  contract_bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_deploy_contract(db, address.data(), contract_hash.data(),
                                  contract_bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_TRUE(has_balance_item(db, address));

  EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
  EXPECT_FALSE(typed_db->is_lifecycle_poisoned());
  EXPECT_TRUE(has_balance_item(db, address));
}

// The reported reproduction: writers and an advance on one DB handle. Under
// ThreadSanitizer an unfixed build reports the put()/sync() race here; on every
// build each call must either succeed or be refused, nothing else.
TEST(TestBindingsTest, SetupWritersRaceAdvanceNewBlock)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer free_db_on_exit(
      [db] { EXPECT_EQ(free_db(db), kTestFFIErrorCodeNone); });
  auto *typed_db = static_cast<DBContext *>(db);

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer close_rc_on_exit(
      [rc] { EXPECT_EQ(close_runtime_context(rc), kTestFFIErrorCodeNone); });

  auto contract = make_registered_contract(rc);
  auto contract_bytes = x23::serialize(contract);
  auto contract_hash = sha256(contract);
  // Created up front so the deploy writer never sees a missing contract row,
  // which is a distinct refusal from the one under test.
  ASSERT_EQ(setup_create_contract(rc, db, contract_bytes.data(),
                                  contract_bytes.size()),
            kTestFFIErrorCodeNone);
  auto grant_address = make_contract_address(0x5a);

  constexpr int kWriterThreads = 3;
  constexpr int kAdvanceIterations = 16;

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> advancer_done{false};

  std::atomic<int> write_attempts{0};
  std::atomic<int> successful_writes{0};
  std::atomic<int> refused_writes{0};
  std::atomic<int> unexpected_write_errors{0};

  auto record = [&](TestFFIErrorCode code) {
    write_attempts.fetch_add(1, std::memory_order_relaxed);
    if (code == kTestFFIErrorCodeNone) {
      successful_writes.fetch_add(1, std::memory_order_relaxed);
    }
    else if (code == kTestFFIErrorCodeUnrecoverableError) {
      // The documented refusal: an advance was in flight.
      refused_writes.fetch_add(1, std::memory_order_relaxed);
    }
    else {
      unexpected_write_errors.fetch_add(1, std::memory_order_relaxed);
    }
    // Back off so the advancer can find a gap between borrows; yield() alone
    // returns straight to this loop on a machine with cores to spare, and the
    // writers then starve the advance (see the #2704 test).
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  };

  auto wait_for_start = [&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  };

  // One writer per mutator.
  std::vector<std::thread> writers;
  writers.reserve(kWriterThreads);
  writers.emplace_back([&] {
    wait_for_start();
    while (!advancer_done.load(std::memory_order_acquire)) {
      record(setup_grant_contract_storage_tokens(db, grant_address.data(), 1));
    }
  });
  writers.emplace_back([&] {
    wait_for_start();
    while (!advancer_done.load(std::memory_order_acquire)) {
      // Rewrites the same contract row; the write is what matters here.
      record(setup_create_contract(rc, db, contract_bytes.data(),
                                   contract_bytes.size()));
    }
  });
  writers.emplace_back([&] {
    wait_for_start();
    uint64_t deployments = 0;
    while (!advancer_done.load(std::memory_order_acquire)) {
      // A fresh address each time, so every deploy is a new metadata row.
      ContractAddress deploy_address = make_contract_address(0xd0);
      std::memcpy(deploy_address.data() + 1, &deployments, sizeof(deployments));
      ++deployments;
      record(setup_deploy_contract(db, deploy_address.data(),
                                   contract_hash.data(),
                                   contract_bytes.size()));
    }
  });

  std::thread advancer([&] {
    wait_for_start();
    for (int i = 0; i < kAdvanceIterations; i++) {
      // A refusal here is the documented outcome when a writer holds a borrow,
      // and nothing below counts landed advances, so there is no retry to get
      // wrong. A poisoned DB reports the same code, though; stop on that and
      // let the assertions after the join name it.
      if (advance_new_block(db, nullptr) != kTestFFIErrorCodeNone &&
          typed_db->is_lifecycle_poisoned()) {
        break;
      }
      // Leave a clear window for the writers between advances.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    advancer_done.store(true, std::memory_order_release);
  });

  while (ready.load(std::memory_order_acquire) < kWriterThreads + 1) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  advancer.join();
  for (auto &writer : writers) {
    writer.join();
  }

  // Under contention neither side is guaranteed to win, so assert what holds
  // whoever does: every write either landed or was refused, nothing else, and
  // the exclusion itself is pinned by the deterministic test above.
  EXPECT_GT(write_attempts.load(), 0);
  EXPECT_EQ(unexpected_write_errors.load(), 0);
  EXPECT_EQ(successful_writes.load() + refused_writes.load(),
            write_attempts.load());
  EXPECT_FALSE(typed_db->has_open_handlers());
  EXPECT_FALSE(typed_db->is_lifecycle_poisoned());

  // Every path still works once the contention is over.
  EXPECT_EQ(setup_grant_contract_storage_tokens(db, grant_address.data(), 1),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_create_contract(rc, db, contract_bytes.data(),
                                  contract_bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(setup_deploy_contract(db, make_contract_address(0xee).data(),
                                  contract_hash.data(), contract_bytes.size()),
            kTestFFIErrorCodeNone);
  EXPECT_EQ(advance_new_block(db, nullptr), kTestFFIErrorCodeNone);
}

} // namespace hogd
