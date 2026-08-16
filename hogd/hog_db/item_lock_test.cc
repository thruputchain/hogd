#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "db_conflict.hh"

#include "address_auth.hh"
#include "auth_claim.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/item_change.h"
#include "hogd/hog_db/item_lock.h"
#include "hogd/hog_db/item_lock/concepts.h"
#include "hogd/hog_db/item_lock/nnint128_lock.h"
#include "hogd/hog_db/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/x23.h"

namespace hogd {

namespace {
ItemChange
make_nnint_change(int128_t set, int128_t add)
{
  return NNInt128(set, add);
}

ItemChange
make_bytestring_change(std::vector<uint8_t> data)
{
  return Bytes(std::as_bytes(std::span(data)));
}

ItemChange
make_rcq_insert_change(uint8_t value)
{
  RetrocausalQueueDelta delta;
  delta.insert.added_entries.push_back(QueueEntry{value});
  return ItemChange(RetrocausalQueueIt(std::move(delta)));
}

ItemChange
make_deletion_change()
{
  return Deletion();
}

DeployedContractHash
make_deployed_hash(uint8_t value, uint32_t contract_size)
{
  DeployedContractHash hash{};
  hash.hash.fill(value);
  hash.contract_size = contract_size;
  hash.contract_type = ContractType::kWasmi;
  return hash;
}

ItemChange
make_deploy_change(uint8_t value, uint32_t contract_size)
{
  return DeployedContractData(make_deployed_hash(value, contract_size));
}

AddressAuthData
make_base_auth_data(
    std::unordered_map<uint32_t, PublicKeyConfigOpt> public_keys)
{
  AddressAuthData base_auth_data{};
  for (auto &[key_id, pk] : public_keys) {
    base_auth_data.public_keys[key_id] = std::move(pk);
  }
  return base_auth_data;
}

AuthDataItem
make_auth_data_change(
    std::unordered_map<uint32_t, PublicKeyConfigOpt> const &public_keys)
{
  return AuthDataItem(make_base_auth_data(public_keys));
}

// Helper to create a PublicKey
PublicKey
make_test_key(uint8_t value)
{
  PublicKey key;
  key.ed25519().fill(value);
  return key;
}

// Helper to create a PublicKeyConfig
PublicKeyConfigOpt
make_test_pk_config(uint8_t value, Weight weight = 100)
{
  PublicKey key = make_test_key(value);
  return x23::Ptr<PublicKeyConfig>(
      new PublicKeyConfig{.key = key, .max_weight = weight});
}

constexpr StorageCostModel kTestCostModel =
    StorageCostModel(12000, 1, 1, 1, 1234);

// Helper to create a dummy LockContext for testing
inline LockContext
make_dummy_context()
{
  // These are never dereferenced in most lock implementations
  static HogDbDAO *dummy_dao = nullptr;
  static ItemKey dummy_key = make_item_key(make_address(0), 0);
  return LockContext(dummy_dao, 0, dummy_key, &kTestCostModel);
}

inline LockContext
make_rcq_context()
{
  static std::optional<Directory> dir;
  static std::optional<HogDbDAO> dao;
  if (!dao.has_value()) {
    auto dir_status = Directory::open_temporary();
    CHECK(static_cast<bool>(dir_status));
    dir.emplace(std::move(*dir_status));

    constexpr size_t kHeapSize = 10_MiB;
    auto db = Database::open(dir->get_path(),
                             Database::estimate_heap_size(kHeapSize), 0);
    CHECK(static_cast<bool>(db));
    dao.emplace(RAMDB(std::move(*db)));
  }

  static ItemKey dummy_key = make_item_key(make_address(0), 0);
  return LockContext(&*dao, 0, dummy_key, &kTestCostModel);
}

} // namespace

TEST(ItemLockTest, BasicUsage)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);

  // conflicting type
  auto conflicting = make_nnint_change(50, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &conflicting, make_dummy_context());
  EXPECT_FALSE(r3.has_value());

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCost)
{
  ItemLock lock(10000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  EXPECT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost - 10000);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  EXPECT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost - 10000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, make_dummy_context());
  EXPECT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.kBaseItemCost - 10000);

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, CommitChangeWithSnapshotCostReleaseNonPermamnentExclusive)
{
  ItemLock lock(kTestCostModel.kBaseItemCost +
                1000); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(100, 20);
  auto third = make_nnint_change(100, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), -1000);

  // storage cost should be deterministic
  auto r4 = lock.try_atomic_change_lock(nullptr, &third, make_dummy_context());
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), 0);

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, NNIntChangeBase)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);

  auto second_compat = make_nnint_change(200, 30);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());

  r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());
  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());
  r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second_compat, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  auto r3 = lock.try_atomic_change_lock(nullptr, &second_compat,
                                        make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.kBaseItemCost);
  EXPECT_EQ(lock.commit_change(second_compat, make_dummy_context()), 0);
}

TEST(ItemLockTest, NNIntChangeExclusiveAfterCommit)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto first2 = make_nnint_change(100, 20);
  auto second = make_nnint_change(200, 20);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  // Now we can acquire a shared lock but not an exclusive lock

  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());

  auto r2 = lock.try_atomic_change_lock(nullptr, &first2, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.kBaseItemCost);

  EXPECT_FALSE(
      lock.try_atomic_change_lock(&first2, &second, make_dummy_context())
          .has_value());

  ItemChange first2_copy = first2;
  EXPECT_TRUE(first2.narrow(second).has_value());

  ASSERT_TRUE(first2.is_exclusive());
  EXPECT_FALSE(
      lock.try_atomic_change_lock(&first2_copy, &first2, make_dummy_context())
          .has_value());
}

TEST(ItemLockTest, ChangeItemType)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_nnint_change(100, 10);
  auto second = make_bytestring_change({1, 2, 3});

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());

  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());
  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_bytestring_cost(3));

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.get_bytestring_cost(3));
}

TEST(ItemLockTest, SeveralBytestringAcquires)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_bytestring_change({1, 2, 3});                 // 3 bytes
  auto second = make_bytestring_change({4, 5, 6, 7, 8});          // 5 bytes
  auto third = make_bytestring_change({7, 8, 9, 10, 11, 12, 13}); // 7 bytes

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_bytestring_cost(3));

  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());
  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_bytestring_cost(5));

  EXPECT_TRUE(third.narrow(second).has_value());
  auto r3 = lock.try_atomic_change_lock(&second, &third, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_bytestring_cost(7));

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()),
            kTestCostModel.get_bytestring_cost(7));

  // same as third
  auto post_commit = make_bytestring_change({7, 8, 9, 10, 11, 12, 13});
  auto r4 =
      lock.try_atomic_change_lock(nullptr, &post_commit, make_dummy_context());
  ASSERT_TRUE(r4.has_value());
  EXPECT_EQ(r4.value(), kTestCostModel.get_bytestring_cost(7));
  EXPECT_EQ(lock.commit_change(post_commit, make_dummy_context()), 0);

  // can't acaquire exclusive lock now
  auto post_commit_exclusive = post_commit;
  EXPECT_TRUE(post_commit_exclusive.narrow(first).has_value());
  ASSERT_TRUE(post_commit_exclusive.is_exclusive());
  ASSERT_TRUE(
      lock.try_atomic_change_lock(nullptr, &post_commit, make_dummy_context())
          .has_value());
  ASSERT_FALSE(lock.try_atomic_change_lock(&post_commit, &post_commit_exclusive,
                                           make_dummy_context())
                   .has_value());
}

TEST(ItemLockTest, PermamentExclusiveInitial)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), -1000);
}

TEST(ItemLockTest, PermamentExclusiveDowngrade)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), -1000);
}

TEST(ItemLockTest, PermamentExclusiveUpgradeDowngrade)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());
  auto third = second;
  second.set_permanent_exclusive();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), -1000);

  auto r3 = lock.try_atomic_change_lock(&second, &third, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), 0);

  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), -1000);
}

TEST(ItemLockTest, PermamentExclusiveKeepsLock)
{
  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto first = make_nnint_change(100, 10);
  first.set_permanent_exclusive();
  auto second = make_nnint_change(100, 20);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -1000);

  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), -1000);

  // second should not conflict with first, except that first is permanent
  // exclusive
  ASSERT_FALSE(
      lock.try_atomic_change_lock(nullptr, &second, make_dummy_context())
          .has_value());
}

TEST(ItemLockTest, DeletionChange)
{
  ItemLock lock(0); // creating item from empty storage slot
  auto first = make_deletion_change();
  auto second = make_deletion_change();
  auto third = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  r1 = lock.try_atomic_change_lock(&first, &third, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(third, make_dummy_context()), 0);
}

TEST(ItemLockTest, DeletionChangeReleaseCost)
{
  ItemLock lock(100); // creating item from empty storage slot
  auto first = make_deletion_change();
  auto second = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), 0);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), 0);

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()), -100);
  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), 0);
}

TEST(ItemLockTest, DeletionChangePermamentExclusiveRelease)
{
  ItemLock lock(100);
  auto first = make_deletion_change();
  first.set_permanent_exclusive();
  auto second = make_deletion_change();

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), -100);

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()), -100);
}

TEST(ItemLockTest, BaseDataConflict)
{
  // In practice, auth data will always have the base data be from the previous
  // snapshot, so it will always be the same. Still, worth checking to make sure
  // we don't have some bizarre errors there.
  ItemLock lock(0);
  auto first = ItemChange(make_auth_data_change({{0, make_test_pk_config(1)}}));
  auto second =
      ItemChange(make_auth_data_change({{1, make_test_pk_config(2)}}));

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(1));

  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_dummy_context());
  ASSERT_FALSE(r2.has_value());

  ASSERT_FALSE(
      lock.try_atomic_change_lock(&first, &second, make_dummy_context())
          .has_value());

  EXPECT_EQ(lock.commit_change(first, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
}

TEST(ItemLockTest, AuthAddKey)
{
  ItemLock lock(0);
  // start with 1 initial key
  auto first = make_auth_data_change({{0, make_test_pk_config(1)}});
  first.set_new_pk(1, make_test_pk_config(2));
  ItemChange first_change(std::move(first));

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2));
}

TEST(ItemLockTest, ChangeKey)
{
  ItemLock lock(0);
  auto first = make_auth_data_change({{0, make_test_pk_config(1)}});
  first.set_new_pk(0, make_test_pk_config(2));
  ItemChange first_change(std::move(first));

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(1));

  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
}

TEST(ItemLockTest, DISABLED_AuthAddKeyMultiple)
{
  ItemLock lock(0);
  auto first = make_auth_data_change({{0, make_test_pk_config(1)}});
  first.set_new_pk(1, make_test_pk_config(2));
  ItemChange first_change(std::move(first));

  // NOLINTBEGIN(performance-unnecessary-copy-initialization)
  ItemChange second_change = first_change;
  ItemChange third_change = first_change;
  // NOLINTEND(performance-unnecessary-copy-initialization)

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  auto r2 = lock.try_atomic_change_lock(nullptr, &second_change,
                                        make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_auth_data_cost(2));

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &third_change, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(second_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2));
  EXPECT_EQ(lock.commit_change(third_change, make_dummy_context()), 0);
  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()), 0);
}

TEST(ItemLockTest, DISABLED_MoveKeys)
{
  // This test starts with 1 initial key.  Each transaction deletes that key and
  // creates another key elsewhere.
  auto pk = make_test_pk_config(1);
  ItemLock lock(0);
  auto first = make_auth_data_change({{0, pk}});
  auto second = make_auth_data_change({{0, pk}});
  auto third = make_auth_data_change({{0, pk}});

  first.set_new_pk(0, nullptr);
  second.set_new_pk(0, nullptr);
  third.set_new_pk(0, nullptr);

  first.set_new_pk(1, make_test_pk_config(2));
  second.set_new_pk(2, make_test_pk_config(2));
  third.set_new_pk(3, make_test_pk_config(2));

  ItemChange first_change(std::move(first));
  ItemChange second_change(std::move(second));
  ItemChange third_change(std::move(third));

  auto r1 =
      lock.try_atomic_change_lock(nullptr, &first_change, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.get_auth_data_cost(2));

  auto r2 = lock.try_atomic_change_lock(nullptr, &second_change,
                                        make_dummy_context());
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value(), kTestCostModel.get_auth_data_cost(2));

  auto r3 =
      lock.try_atomic_change_lock(nullptr, &third_change, make_dummy_context());
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.get_auth_data_cost(2));

  EXPECT_EQ(lock.commit_change(second_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(1));
  EXPECT_EQ(lock.commit_change(third_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(2) -
                kTestCostModel.get_auth_data_cost(1));
  EXPECT_EQ(lock.commit_change(first_change, make_dummy_context()),
            kTestCostModel.get_auth_data_cost(3) -
                kTestCostModel.get_auth_data_cost(2));
}

TEST(ItemLockTest, FinalizeExclusive)
{
  // Reproducing for clarity an error case from
  // TryInvokeTest.NNIntAddSimpleInterleavings
  ItemLock lock(0);
  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_dummy_context());
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1.value(), kTestCostModel.kBaseItemCost);

  auto r2 = lock.try_atomic_change_lock(&first, &second, make_dummy_context());
  ASSERT_TRUE(r2.has_value());

  EXPECT_EQ(lock.commit_change(second, make_dummy_context()),
            kTestCostModel.kBaseItemCost);

  auto const *inner = lock.get_inner<NNInt128Lock>();
  ASSERT_TRUE(inner != nullptr);
  EXPECT_EQ(inner->get_base_and_delta(),
            (std::pair<int128_t, int128_t>(200, 20)));
}

// NOLINTBEGIN(bugprone-unchecked-optional-access)
TEST(ItemLockTest, ConcurrentRCQReadReturnsNulloptUntilCommit)
{
  ItemLock lock(0);
  auto first = make_rcq_insert_change(0x01);
  auto second = make_rcq_insert_change(0x02);

  auto r1 = lock.try_atomic_change_lock(nullptr, &first, make_rcq_context());
  ASSERT_TRUE(r1.has_value());
  auto r2 = lock.try_atomic_change_lock(nullptr, &second, make_rcq_context());
  ASSERT_TRUE(r2.has_value());

  EXPECT_EQ(lock.get_concurrent_entry(0), std::nullopt);
  EXPECT_EQ(lock.get_concurrent_entry(1), std::nullopt);

  EXPECT_EQ(lock.commit_change(first, make_rcq_context()),
            kTestCostModel.get_retrocausal_queue_cost(1));

  auto entry0 = lock.get_concurrent_entry(0);
  ASSERT_TRUE(entry0.has_value());
  EXPECT_EQ(*entry0, QueueEntry{0x01});
  EXPECT_EQ(lock.get_concurrent_entry(1), std::nullopt);

  EXPECT_EQ(lock.commit_change(second, make_rcq_context()), 1);

  auto entry0_after = lock.get_concurrent_entry(0);
  ASSERT_TRUE(entry0_after.has_value());
  EXPECT_EQ(*entry0_after, QueueEntry{0x01});

  auto entry1 = lock.get_concurrent_entry(1);
  ASSERT_TRUE(entry1.has_value());
  EXPECT_EQ(*entry1, QueueEntry{0x02});
}

TEST(ItemLockTest, ConcurrentRCQReadDoesNotTouchUncommittedVariant)
{
  for (int iter = 0; iter < 1000; ++iter) {
    ItemLock lock(0);
    std::atomic<bool> stop = false;
    std::atomic<bool> writer_failed = false;

    std::thread reader([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        auto entry = lock.get_concurrent_entry(0);
        (void)entry;
      }
    });

    std::thread writer([&]() {
      auto ctx = make_rcq_context();
      auto finish = [&]() { stop.store(true, std::memory_order_relaxed); };
      auto guard = x23::Defer(finish);

      for (int j = 0; j < 200; ++j) {
        auto change = make_rcq_insert_change(0x42);
        auto res = lock.try_atomic_change_lock(nullptr, &change, ctx);
        if (!res.has_value()) {
          continue;
        }
        auto release = lock.try_atomic_change_lock(&change, nullptr, ctx);
        if (!release.has_value()) {
          writer_failed.store(true, std::memory_order_relaxed);
          ADD_FAILURE() << "RCQ release path unexpectedly failed";
          return;
        }
      }
    });

    reader.join();
    writer.join();
    ASSERT_FALSE(writer_failed.load(std::memory_order_relaxed));
  }
}
// NOLINTEND(bugprone-unchecked-optional-access)

TEST(ItemLockTest, FinalizedFastPathRelaxedLoadDataRace)
{
  for (int i = 0; i < 10000; ++i) {
    ItemLock lock(0);
    auto change_a = make_nnint_change(100, 10);
    auto change_b = make_nnint_change(100, 20);

    std::atomic<bool> finalized_ready = false;
    std::atomic<bool> committed = false;

    std::thread thread_a([&]() {
      auto ctx = make_dummy_context();
      auto res = lock.try_atomic_change_lock(nullptr, &change_a, ctx);
      if (res.has_value()) {
        committed.store(true, std::memory_order_relaxed);
        ASSERT_TRUE(lock.commit_change(change_a, ctx).has_value());
      }
      finalized_ready.store(true, std::memory_order_relaxed);
    });

    std::thread thread_b([&]() {
      while (!finalized_ready.load(std::memory_order_relaxed)) {
      }
      auto ctx = make_dummy_context();
      auto ignored = lock.try_atomic_change_lock(nullptr, &change_b, ctx);
      (void)ignored;
    });

    thread_a.join();
    thread_b.join();
    ASSERT_TRUE(committed.load(std::memory_order_relaxed));
  }
}

TEST(ItemLockTest, EmptyRCQEntryCommitDoesNotCrash)
{
  RetrocausalQueueDelta delta;
  QueueEntry empty_entry;
  ASSERT_EQ(empty_entry.size(), 0U);
  ASSERT_EQ(empty_entry.data(), nullptr);
  delta.insert.added_entries.push_back(empty_entry);

  ItemChange change(RetrocausalQueueIt(std::move(delta)));

  ItemLock lock(0);
  auto ctx = make_rcq_context();
  auto lock_status = lock.try_atomic_change_lock(nullptr, &change, ctx);
  ASSERT_TRUE(lock_status.has_value());

  auto cost = lock.commit_change(change, ctx);
  EXPECT_EQ(cost, kTestCostModel.get_retrocausal_queue_cost(1));
}

TEST(ItemLockTest, RCQNarrowAcceptsShrinkingInserts)
{
  // Sub1 inserts [A, B], sub2 inserts [B] (self-consumed A).
  // narrow() must accept the shrink.
  RetrocausalQueueDelta delta1;
  delta1.insert.added_entries.push_back(QueueEntry{0x0A});
  delta1.insert.added_entries.push_back(QueueEntry{0x0B});
  auto first = ItemChange(RetrocausalQueueIt(std::move(delta1)));

  RetrocausalQueueDelta delta2;
  delta2.insert.added_entries.push_back(QueueEntry{0x0B});
  auto second = ItemChange(RetrocausalQueueIt(std::move(delta2)));

  auto result = second.narrow(first);
  EXPECT_TRUE(result.has_value());
}

TEST(ItemLockTest, RCQNarrowAcceptsShrinkToEmpty)
{
  // Sub1 inserts [A], sub2 inserts nothing (fully self-consumed).
  RetrocausalQueueDelta delta1;
  delta1.insert.added_entries.push_back(QueueEntry{0x0A});
  auto first = ItemChange(RetrocausalQueueIt(std::move(delta1)));

  RetrocausalQueueDelta delta2;
  auto second = ItemChange(RetrocausalQueueIt(std::move(delta2)));

  auto result = second.narrow(first);
  EXPECT_TRUE(result.has_value());
}

TEST(ItemLockTest, RCQNarrowMaxInsertCountTracksAcrossMultipleNarrows)
{
  // Sub1: 3 inserts, Sub2: 1 insert (shrink), Sub3: 4 inserts (grow past peak).
  RetrocausalQueueDelta d1;
  d1.insert.added_entries.push_back(QueueEntry{0x01});
  d1.insert.added_entries.push_back(QueueEntry{0x02});
  d1.insert.added_entries.push_back(QueueEntry{0x03});
  auto c1 = ItemChange(RetrocausalQueueIt(std::move(d1)));

  RetrocausalQueueDelta d2;
  d2.insert.added_entries.push_back(QueueEntry{0x03});
  auto c2 = ItemChange(RetrocausalQueueIt(std::move(d2)));

  auto r1 = c2.narrow(c1);
  ASSERT_TRUE(r1.has_value());

  RetrocausalQueueDelta d3;
  d3.insert.added_entries.push_back(QueueEntry{0x03});
  d3.insert.added_entries.push_back(QueueEntry{0x04});
  d3.insert.added_entries.push_back(QueueEntry{0x05});
  d3.insert.added_entries.push_back(QueueEntry{0x06});
  auto c3 = ItemChange(RetrocausalQueueIt(std::move(d3)));

  auto r2 = c3.narrow(c2);
  ASSERT_TRUE(r2.has_value());
}

TEST(ItemLockTest, ConcurrentNonexclusiveCommitSingleFinalizer)
{
  constexpr size_t kThreads = 64;
  constexpr size_t kAttempts = 500;
  constexpr int128_t kSnapshotCost = 50'000;

  auto ctx = make_dummy_context();

  for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
    ItemLock lock(kSnapshotCost);

    auto deletion = make_deletion_change();
    for (size_t i = 0; i < kThreads; ++i) {
      auto res = lock.try_atomic_change_lock(nullptr, &deletion, ctx);
      ASSERT_TRUE(res.has_value());
      ASSERT_EQ(*res, 0);
    }

    std::barrier sync(static_cast<std::ptrdiff_t>(kThreads));
    std::vector<int128_t> costs(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (size_t i = 0; i < kThreads; ++i) {
      threads.emplace_back([&, i]() {
        sync.arrive_and_wait();
        auto r = lock.commit_change(deletion, ctx);
        ASSERT_TRUE(r.has_value());
        costs[i] = *r;
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    int finalizer_count = 0;
    for (size_t i = 0; i < kThreads; ++i) {
      if (costs[i] == -kSnapshotCost) {
        ++finalizer_count;
      }
      else {
        EXPECT_EQ(costs[i], 0) << "attempt=" << attempt << " thread=" << i;
      }
    }
    EXPECT_EQ(finalizer_count, 1) << "attempt=" << attempt;
  }
}

TEST(ItemLockTest, ExclusiveCommitBasic)
{
  auto ctx = make_dummy_context();

  auto first = make_nnint_change(100, 10);
  auto second = make_nnint_change(200, 20);
  EXPECT_TRUE(second.narrow(first).has_value());
  ASSERT_TRUE(second.is_exclusive());

  ItemLock lock(0);
  auto r1 = lock.try_atomic_change_lock(nullptr, &first, ctx);
  ASSERT_TRUE(r1.has_value());
  auto r2 = lock.try_atomic_change_lock(&first, &second, ctx);
  ASSERT_TRUE(r2.has_value());

  EXPECT_EQ(lock.commit_change(second, ctx), kTestCostModel.kBaseItemCost);
}

TEST(ItemLockTest, PermanentExclusiveCommitBasic)
{
  auto ctx = make_dummy_context();

  auto change = make_nnint_change(100, 10);
  change.set_permanent_exclusive();
  ASSERT_TRUE(change.is_permanent_exclusive());

  ItemLock lock(kTestCostModel.kBaseItemCost + 1000);
  auto r = lock.try_atomic_change_lock(nullptr, &change, ctx);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, -1000);

  EXPECT_EQ(lock.commit_change(change, ctx), -1000);
}

TEST(ItemLockTest, ExclusiveCommitWhileReservationsRace)
{
  constexpr size_t kReaders = 32;
  constexpr size_t kAttempts = 200;

  auto ctx = make_dummy_context();

  for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
    ItemLock lock(0);

    auto first = make_nnint_change(100, 10);
    auto second = make_nnint_change(200, 20);
    EXPECT_TRUE(second.narrow(first).has_value());
    ASSERT_TRUE(second.is_exclusive());

    auto r1 = lock.try_atomic_change_lock(nullptr, &first, ctx);
    ASSERT_TRUE(r1.has_value());
    auto r2 = lock.try_atomic_change_lock(&first, &second, ctx);
    ASSERT_TRUE(r2.has_value());

    std::atomic<bool> stop = false;
    std::atomic<size_t> reservation_attempts = 0;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);

    for (size_t i = 0; i < kReaders; ++i) {
      readers.emplace_back([&]() {
        auto probe = make_nnint_change(50, 5);
        while (!stop.load(std::memory_order_acquire)) {
          auto res = lock.try_atomic_change_lock(nullptr, &probe, ctx);
          reservation_attempts.fetch_add(1, std::memory_order_relaxed);
          if (res.has_value()) {
            [[maybe_unused]] auto release =
                lock.try_atomic_change_lock(&probe, nullptr, ctx);
          }
        }
      });
    }

    auto cost = lock.commit_change(second, ctx);
    stop.store(true, std::memory_order_release);

    for (auto &t : readers) {
      t.join();
    }

    EXPECT_EQ(cost, kTestCostModel.kBaseItemCost) << "attempt=" << attempt;
  }
}

TEST(ItemLockTest, PermanentExclusiveCommitWhileReservationsRace)
{
  constexpr size_t kReaders = 32;
  constexpr size_t kAttempts = 200;

  auto ctx = make_dummy_context();

  for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
    ItemLock lock(kTestCostModel.kBaseItemCost + 1000);

    auto change = make_nnint_change(100, 10);
    change.set_permanent_exclusive();

    auto r = lock.try_atomic_change_lock(nullptr, &change, ctx);
    ASSERT_TRUE(r.has_value());

    std::atomic<bool> stop = false;
    std::vector<std::thread> readers;
    readers.reserve(kReaders);

    for (size_t i = 0; i < kReaders; ++i) {
      readers.emplace_back([&]() {
        auto probe = make_nnint_change(50, 5);
        while (!stop.load(std::memory_order_acquire)) {
          auto res = lock.try_atomic_change_lock(nullptr, &probe, ctx);
          if (res.has_value()) {
            [[maybe_unused]] auto release =
                lock.try_atomic_change_lock(&probe, nullptr, ctx);
          }
        }
      });
    }

    auto cost = lock.commit_change(change, ctx);
    stop.store(true, std::memory_order_release);

    for (auto &t : readers) {
      t.join();
    }

    EXPECT_EQ(cost, -1000) << "attempt=" << attempt;
  }
}

TEST(ItemLockTest, StaleUncommittedReadCannotBypassPermanentExclusive)
{
  constexpr size_t kPayloadBytes = 1024;
  constexpr size_t kAttempts = 500;

  auto const hardware_threads =
      std::max(4U, std::thread::hardware_concurrency());
  auto const racers = std::min(64U, hardware_threads * 4U);

  std::vector<uint8_t> payload(kPayloadBytes, 0x5a);
  auto ctx = make_dummy_context();

  for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
    ItemLock lock(0);
    auto exclusive = make_bytestring_change(payload);
    exclusive.set_permanent_exclusive();

    auto initial_lock = lock.try_atomic_change_lock(nullptr, &exclusive, ctx);
    ASSERT_TRUE(initial_lock.has_value());

    std::atomic<bool> any_accepted{false};
    std::atomic<bool> stop{false};
    std::barrier<> start(static_cast<std::ptrdiff_t>(racers + 1));

    std::vector<std::thread> threads;
    threads.reserve(racers);
    for (unsigned i = 0; i < racers; ++i) {
      threads.emplace_back([&]() {
        auto nonexclusive = make_bytestring_change(payload);
        start.arrive_and_wait();

        while (!stop.load(std::memory_order_acquire)) {
          auto res = lock.try_atomic_change_lock(nullptr, &nonexclusive, ctx);
          if (res.has_value()) {
            any_accepted.store(true, std::memory_order_release);
            stop.store(true, std::memory_order_release);
            return;
          }

          if (lock.is_finalized()) {
            return;
          }
          std::this_thread::yield();
        }
      });
    }

    start.arrive_and_wait();
    auto commit_res = lock.commit_change(exclusive, ctx);
    ASSERT_TRUE(commit_res.has_value());

    stop.store(true, std::memory_order_release);
    for (auto &thread : threads) {
      thread.join();
    }

    EXPECT_FALSE(any_accepted.load())
        << "stale nonexclusive reservation accepted after permanent-exclusive "
           "finalization on attempt "
        << attempt;
  }
}

// Regression test for issue #3152: a failed RCQ exclusive upgrade used to
// call set_exclusive_lock() -- replacing lock_contents_ with an ExclusiveLock
// -- before the fallible exclusive validation, and never restored the prior
// DeletionLock when validation failed. That poisoned the pending lock: the
// transaction still retained its deletion change while the lock variant had
// become an ExclusiveLock, so a later deletion modification spuriously
// conflicted. The upgrade must now be failure-atomic: on a rejected change
// the lock is left equivalent to its prior deletion reservation.
TEST(ItemLockTest, FailedRCQExclusiveUpgradeIsFailureAtomic)
{
  auto dir_status = Directory::open_temporary();
  ASSERT_TRUE(dir_status.has_value());
  auto dir = std::move(*dir_status);

  auto db_status =
      Database::open(dir.get_path(), Database::estimate_heap_size(10_MiB), 0);
  ASSERT_TRUE(db_status.has_value());
  HogDbDAO dao(RAMDB(std::move(*db_status)));

  auto key = make_item_key(make_address(42), 3152);
  QueueEntry consumed_entry{0x11};
  QueueEntry inserted_entry{0x22};

  RetrocausalQueueMetadata metadata{};
  metadata.live_entries = 1;
  metadata.max_live_block_num = 1;
  RetrocausalQueueBlock block{};
  block.block_number = 1;
  block.entries.push_back(consumed_entry);
  ASSERT_TRUE(dao.write_retrocausal_queue_metadata(key, metadata).has_value());
  ASSERT_TRUE(dao.write_retrocausal_queue_block(key, block).has_value());
  ASSERT_TRUE(dao.commit().has_value());

  const int128_t snapshot_cost = kTestCostModel.get_retrocausal_queue_cost(1);
  LockContext ctx(&dao, 1, key, &kTestCostModel);

  ItemLock lock({snapshot_cost, ItemLockCacheData::from_rcq()});
  auto deletion = make_deletion_change();
  ASSERT_TRUE(lock.try_atomic_change_lock(nullptr, &deletion, ctx).has_value());

  // Build a recreated-RCQ change whose current-block consume references a
  // reloaded snapshot entry that this delta does not add. Narrowing over the
  // pending deletion makes the change exclusive (a type change).
  RetrocausalQueueDelta delta;
  delta.insert.added_entries.push_back(inserted_entry);
  delta.consume =
      x23::Ptr<RetrocausalQueueConsumeDelta>(new RetrocausalQueueConsumeDelta{
          .past_block_consume = {.max_fully_consumed_block_number = 1,
                                 .partial_consumed_number = 0},
          .current_block_consumed = {consumed_entry}});

  ItemChange recreated_rcq(RetrocausalQueueIt(std::move(delta)));
  ASSERT_TRUE(recreated_rcq.narrow(deletion).has_value());
  ASSERT_TRUE(recreated_rcq.is_exclusive());

  // The exclusive upgrade is rejected by RCQ exclusive validation (a
  // current-block consumed entry that this delta does not add).
  auto upgrade = lock.try_atomic_change_lock(&deletion, &recreated_rcq, ctx);
  ASSERT_FALSE(upgrade.has_value());
  EXPECT_EQ(upgrade.error(), HogDbError{HogDbCode::kInvalidExecutorBehavior});

  // The lock must not be poisoned: the prior deletion reservation is intact,
  // so re-applying a deletion modification succeeds (as it would on a fresh
  // lock) instead of seeing a spurious conflict.
  auto retained_deletion = make_deletion_change();
  auto retry = lock.try_atomic_change_lock(&deletion, &retained_deletion, ctx);
  EXPECT_TRUE(retry.has_value());
}

// Coverage for the crash class of hogd issue #1641 (filed against the legacy
// db_interface, wontfixed there because db_interface is replaced by hog_db).
// In db_interface, the exclusive-lock reservation path accepted NNInt deltas
// without semantic validation, and the commit-time downgrade asserted that
// replaying the deferred delta could not fail -- so a semantically invalid
// exclusive NNInt delta aborted the node at commit (assert(res) in Debug,
// std::bad_variant_access in Release).
//
// hog_db has the same structural shape: exclusive acquisition only prices the
// change (ExclusiveLock::try_atomic_change_lock performs no type-specific
// validation), and the type-specific check runs at commit time, when
// ItemLock::commit_change re-prepares the typed lock and runs a fallible
// admission. The invariant this test pins is that a failed admission surfaces
// as a structured error -- never an assert or an unchecked variant access.
// Covers all three NNInt variants from the issue: negative add against
// implicit base 0 (fresh key), negative add against a negative base, and
// explicit set-add overflow past INT128_MAX.
//
// In production these changes cannot reach an ItemLock, because
// InvokeTX::request_nnint128 validates the delta before constructing the
// change (regardless of the ExclusiveLock flag); this test hand-builds them
// to prove the commit path stays structured even if a caller bypasses that
// validation.
TEST(ItemLockTest, ExclusiveNNIntFailedCommitAdmissionReturnsError)
{
  struct Case {
    const char *name;
    int128_t base;
    int128_t delta;
  };
  const Case cases[] = {
      {.name = "fresh key: implicit base 0, add -1", .base = 0, .delta = -1},
      {.name = "negative base -1, add -1", .base = -1, .delta = -1},
      {.name = "set INT128_MAX, add 1 overflows",
       .base = std::numeric_limits<int128_t>::max(),
       .delta = 1},
  };

  for (const auto &c : cases) {
    SCOPED_TRACE(c.name);

    // Control: the ordinary (non-exclusive) reservation path rejects the
    // same delta outright, proving it is semantically invalid.
    {
      ItemLock lock(0);
      auto change = make_nnint_change(c.base, c.delta);
      std::optional<DbConflictCode> reason;
      auto ctx = make_dummy_context();
      ctx.conflict_reason = &reason;
      auto res = lock.try_atomic_change_lock(nullptr, &change, ctx);
      ASSERT_FALSE(res.has_value());
      EXPECT_EQ(res.error(), HogDbError{HogDbCode::kConflict});
      EXPECT_EQ(reason, DbConflictCode::kDbConflictBoundsExceeded);
    }

    // Exclusive acquisition accepts the same change without semantic
    // validation -- the same precondition that made the db_interface bug
    // reachable.
    ItemLock lock(0);
    auto change = make_nnint_change(c.base, c.delta);
    change.set_permanent_exclusive();
    auto acquired =
        lock.try_atomic_change_lock(nullptr, &change, make_dummy_context());
    ASSERT_TRUE(acquired.has_value());

    // Commit-time admission replays the delta and must fail with a
    // structured conflict, leaving the lock unfinalized. In db_interface the
    // equivalent path was assert(res) / std::bad_variant_access.
    std::optional<DbConflictCode> reason;
    auto ctx = make_dummy_context();
    ctx.conflict_reason = &reason;
    auto commit_res = lock.commit_change(change, ctx);
    ASSERT_FALSE(commit_res.has_value());
    EXPECT_EQ(commit_res.error(), HogDbError{HogDbCode::kConflict});
    EXPECT_EQ(reason, DbConflictCode::kDbConflictBoundsExceeded);
    EXPECT_FALSE(lock.is_finalized());
  }
}

// Companion to the issue #1641 coverage above: the pre-commit downgrade path.
// Within a transaction an NNInt reservation can go regular -> exclusive
// (narrow() promotes on a conflicting base, as a savepoint's writes can) and
// back to regular when rollback_to_savepoint restores the pre-savepoint
// change via modify_lock(exclusive_before, regular_after). This is the hog_db
// analog of db_interface's revert_exclusive_lock_to_set_typeclass, whose
// unvalidated replay was the #1641 crash site. Pins that the downgrade
// replaces the ExclusiveLock variant with a fresh typed lock, re-admits the
// restored change from scratch, and leaves a fully regular lock: a second
// holder can reserve alongside and the change commits non-exclusively.
TEST(ItemLockTest, ExclusiveNNIntDowngradeToRegularRevalidates)
{
  auto ctx = make_dummy_context();
  ItemLock lock(0);

  auto regular = make_nnint_change(100, 10);
  auto r1 = lock.try_atomic_change_lock(nullptr, &regular, ctx);
  ASSERT_TRUE(r1.has_value());

  // A conflicting base forces narrow() to promote to exclusive -- the same
  // implicit promotion a savepoint's later writes produce.
  auto exclusive = make_nnint_change(200, 20);
  EXPECT_TRUE(exclusive.narrow(regular).has_value());
  ASSERT_TRUE(exclusive.is_exclusive());
  auto r2 = lock.try_atomic_change_lock(&regular, &exclusive, ctx);
  ASSERT_TRUE(r2.has_value());

  // While exclusive, no other transaction can reserve.
  auto peer = make_nnint_change(100, 5);
  {
    std::optional<DbConflictCode> reason;
    auto peer_ctx = make_dummy_context();
    peer_ctx.conflict_reason = &reason;
    auto blocked = lock.try_atomic_change_lock(nullptr, &peer, peer_ctx);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(reason, DbConflictCode::kDbConflictExclusiveLock);
  }

  // Downgrade: restore the original regular change, as rollback_to_savepoint
  // does. The re-admission runs from scratch and returns a fresh cost bound.
  auto r3 = lock.try_atomic_change_lock(&exclusive, &regular, ctx);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3.value(), kTestCostModel.kBaseItemCost);

  // The lock is a regular NNInt lock again: a second holder with the same
  // base can now reserve alongside, which the ExclusiveLock variant would
  // have rejected.
  auto r4 = lock.try_atomic_change_lock(nullptr, &peer, ctx);
  ASSERT_TRUE(r4.has_value());
  auto released = lock.try_atomic_change_lock(&peer, nullptr, ctx);
  ASSERT_TRUE(released.has_value());

  // The restored change commits through the ordinary non-exclusive path.
  auto commit_res = lock.commit_change(regular, ctx);
  ASSERT_TRUE(commit_res.has_value());
  EXPECT_EQ(*commit_res, kTestCostModel.kBaseItemCost);

  auto const *inner = lock.get_inner<NNInt128Lock>();
  ASSERT_TRUE(inner != nullptr);
  EXPECT_EQ(inner->get_base_and_delta(),
            (std::pair<int128_t, int128_t>(100, 10)));
}

// Issue #1189: a DeployContract change can now carry the exclusive-lock flag,
// which routes it through ExclusiveLock -- a path that previously reached
// panic("unimplemented") because it priced only the bytes/nnint/deletion/
// auth/rcq item types. Pins that the deploy change is admitted, priced
// identically to the regular NewDeploymentLock reservation, and locks
// everyone else out both before and after it finalizes.
TEST(ItemLockTest, ExclusiveDeployContractIsPricedAndBlocksPeers)
{
  constexpr uint32_t kContractSize = 40;
  const int128_t kDeployCost = kTestCostModel.get_deployment_cost(
      make_deployed_hash(0xD0, kContractSize));
  // Guard the fixture rather than the code under test: a free deployment
  // would make the cost assertions below vacuous.
  ASSERT_GT(kDeployCost, 0);

  ItemLock lock(0);
  auto exclusive = make_deploy_change(0xD0, kContractSize);
  exclusive.set_permanent_exclusive();

  auto acquired =
      lock.try_atomic_change_lock(nullptr, &exclusive, make_dummy_context());
  ASSERT_TRUE(acquired.has_value());
  EXPECT_EQ(acquired.value(), kDeployCost);

  // A peer deploying the very same hash -- which the regular
  // NewDeploymentLock admits alongside -- is now locked out.
  {
    auto peer = make_deploy_change(0xD0, kContractSize);
    std::optional<DbConflictCode> reason;
    auto peer_ctx = make_dummy_context();
    peer_ctx.conflict_reason = &reason;
    auto blocked = lock.try_atomic_change_lock(nullptr, &peer, peer_ctx);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), HogDbError{HogDbCode::kConflict});
    EXPECT_EQ(reason, DbConflictCode::kDbConflictExclusiveLock);
  }

  // Committing installs a real NewDeploymentLock and finalizes permanently.
  auto committed = lock.commit_change(exclusive, make_dummy_context());
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(committed.value(), kDeployCost);
  EXPECT_TRUE(lock.is_finalized());
  ASSERT_TRUE(lock.get_inner<NewDeploymentLock>() != nullptr);

  // The permanent exclusive lock outlives the commit: nothing else in the
  // block may touch the key, not even an identical deploy.
  {
    auto peer = make_deploy_change(0xD0, kContractSize);
    std::optional<DbConflictCode> reason;
    auto peer_ctx = make_dummy_context();
    peer_ctx.conflict_reason = &reason;
    auto blocked = lock.try_atomic_change_lock(nullptr, &peer, peer_ctx);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(reason, DbConflictCode::kDbConflictExclusiveLock);
  }
}

// The exclusive upgrade is refused while another transaction already holds a
// (regular) reservation on the deployment key -- the contended direction of
// the case above.
TEST(ItemLockTest, ExclusiveDeployContractBlockedByExistingHolder)
{
  ItemLock lock(0);
  auto held = make_deploy_change(0xD3, 8);
  auto r1 = lock.try_atomic_change_lock(nullptr, &held, make_dummy_context());
  ASSERT_TRUE(r1.has_value());

  auto exclusive = make_deploy_change(0xD3, 8);
  exclusive.set_permanent_exclusive();
  std::optional<DbConflictCode> reason;
  auto ctx = make_dummy_context();
  ctx.conflict_reason = &reason;
  auto blocked = lock.try_atomic_change_lock(nullptr, &exclusive, ctx);
  ASSERT_FALSE(blocked.has_value());
  EXPECT_EQ(reason, DbConflictCode::kDbConflictExclusiveLock);

  // The refusal is a no-op: the original holder still commits normally.
  auto committed = lock.commit_change(held, make_dummy_context());
  ASSERT_TRUE(committed.has_value());
  EXPECT_TRUE(lock.is_finalized());
}

// A permanent exclusive lock reports the true (signed) marginal cost rather
// than clamping at zero, so a deployment that shrinks an address's stored
// contract refunds the difference. Pins that deploy changes take the same
// snapshot_cost_transform branch as the other exclusive item types.
TEST(ItemLockTest, ExclusiveDeployContractReportsRefundAgainstSnapshotCost)
{
  constexpr uint32_t kSmallSize = 4;
  const int128_t kDeployCost =
      kTestCostModel.get_deployment_cost(make_deployed_hash(0xD4, kSmallSize));
  const int128_t kSnapshotCost = kDeployCost + 5000;

  ItemLock lock(kSnapshotCost);
  auto exclusive = make_deploy_change(0xD4, kSmallSize);
  exclusive.set_permanent_exclusive();

  auto acquired =
      lock.try_atomic_change_lock(nullptr, &exclusive, make_dummy_context());
  ASSERT_TRUE(acquired.has_value());
  EXPECT_EQ(acquired.value(), -5000);

  auto committed = lock.commit_change(exclusive, make_dummy_context());
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(committed.value(), -5000);
}

} // namespace hogd
