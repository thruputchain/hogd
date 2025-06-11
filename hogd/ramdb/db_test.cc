#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/random_bytes.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

namespace {

// the number of buckets in log2(n)
constexpr int kBucketOrder = 7; // 128 buckets
// the size of each key to be inserted
constexpr size_t kKeySize = 256;
// the size of each value to be inserted
constexpr size_t kValueSize = 64;
// the maximum size value that can be inserted.
constexpr size_t kMaxValueSize =
    kBigItemThreshold - sizeof(ItemHeader) - kKeySize;
// the number of items to insert
constexpr size_t kInsertions = 32'768;
// the number of times to force a compaction
constexpr int kCompactionIterations = 8;

constexpr std::string
to_string(std::span<const std::byte> bytes)
{
  return std::string(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
}

} // namespace

class DatabaseTest : public testing::Test {
protected:
  DatabaseTest() = default;
  ~DatabaseTest() override = default;

  void SetUp() override
  {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    ASSERT_TRUE(static_cast<bool>(dir));
    dir_ = *dir;
    info("creating database at '{}'", dir_.get_path());
    DBStatus<void> ret = open_db();
    ASSERT_TRUE(static_cast<bool>(ret));
  }

  DBStatus<void>
  open_db(uint64_t target_lsn = std::numeric_limits<uint64_t>::max());
  void insert_n(size_t count);
  DBError insert_until_failure();
  void remove_every_n(int n);
  void check_integrity();

  Directory dir_;
  std::unique_ptr<Database> db_;
  std::unordered_map<std::string, std::string> oracle_;
  RandomBytes rand_;
};

DBStatus<void>
DatabaseTest::open_db(uint64_t target_lsn)
{
  size_t heap_size =
      Database::estimate_heap_size(kInsertions * 2, kKeySize, kValueSize);
  DBStatus<std::unique_ptr<Database>> ret =
      target_lsn == std::numeric_limits<uint64_t>::max()
          ? Database::open(dir_.get_path(), heap_size, kBucketOrder)
          : Database::open_lsn(dir_.get_path(), target_lsn, heap_size,
                               kBucketOrder);
  if (!ret)
    return ret.error();
  db_ = std::move(*ret);
  return {};
}

void
DatabaseTest::insert_n(size_t count)
{
  using KVPair = std::pair<std::string, std::string>;
  std::vector<KVPair> inserts;
  inserts.reserve(count);

  // generate random data to insert
  for (size_t i = 0; i < count; ++i) {
    std::string key(kKeySize, '0');
    rand_(std::span(key));
    std::string value(kValueSize, '0');
    rand_(std::span(value));
    inserts.emplace_back(std::move(key), std::move(value));
  }

  // perform concurrent insertions
  WorkerPool::get_default_pool().for_each_parallel(
      inserts, [this](const std::pair<const std::string, std::string> &val) {
        DBStatus<void> ret = db_->put(std::as_bytes(std::span(val.first)), 0,
                                      std::as_bytes(std::span(val.second)));
        EXPECT_TRUE(ret);
      });

  // update the oracle
  for (auto &[key, value] : inserts) {
    oracle_.emplace(std::move(key), std::move(value));
  }
}

DBError
DatabaseTest::insert_until_failure()
{
  std::string key(kKeySize, '0');
  std::string value(kMaxValueSize, '0');

  while (true) {
    rand_(std::span(key));
    rand_(std::span(value));
    DBStatus<void> ret = db_->put(std::as_bytes(std::span(key)), 0,
                                  std::as_bytes(std::span(value)));
    if (!ret)
      return ret.error();
  }
  std::unreachable();
}

void
DatabaseTest::remove_every_n(int n)
{
  size_t pos = 0;
  for (auto it = oracle_.begin(); it != oracle_.end();) {
    if (pos++ % n != 0) {
      ++it;
      continue;
    }

    DBStatus<void> ret = db_->remove(std::as_bytes(std::span(it->first)));
    EXPECT_TRUE(ret);
    it = oracle_.erase(it);
  }
}

namespace {
template<typename T>
T
subtract(const T &left, const T &right)
{
  auto diff = left;
  for (const auto &kvpair : right) {
    if (diff.find(kvpair.first) != diff.end())
      diff.erase(kvpair.first);
  }
  return diff;
}
} // namespace

void
DatabaseTest::check_integrity()
{
  std::unordered_map<std::string, std::string> tmp;
  db_->get_each([&tmp](std::span<const std::byte> key, int type,
                       std::span<const std::byte> val) {
    auto [it, inserted] = tmp.insert({to_string(key), to_string(val)});
    ASSERT_TRUE(inserted); // checks for duplicates
    ASSERT_EQ(type, 0);    // type should always be 0
  });

  if (tmp != oracle_) {
    auto missing = subtract(oracle_, tmp);
    if (!missing.empty())
      FAIL() << std::format(
          "db missing {} values in oracle, db size {}, oracle size {}",
          missing.size(), tmp.size(), oracle_.size());
    auto extra = subtract(tmp, oracle_);
    if (!extra.empty())
      FAIL() << std::format(
          "db has {} extra values not in oracle, db size {}, oracle size {}",
          missing.size(), tmp.size(), oracle_.size());
  }

  ASSERT_EQ(tmp, oracle_) << "db doesn't match oracle";
}

TEST_F(DatabaseTest, SimpleInsertions)
{
  insert_n(kInsertions);
  check_integrity();
}

TEST_F(DatabaseTest, InsertUntilOOM)
{
  DBError err = insert_until_failure();
  EXPECT_EQ(err, DBCode::kNoMemory);
}

TEST_F(DatabaseTest, ConcurrentInsertions)
{
  insert_n(kInsertions);
  check_integrity();
}

TEST_F(DatabaseTest, InsertionsAndRemovals)
{
  insert_n(kInsertions);
  check_integrity();
  remove_every_n(2);
  check_integrity();
}

TEST_F(DatabaseTest, SyncAndLoad)
{
  // make changes to items.
  insert_n(kInsertions);
  check_integrity();
  remove_every_n(3);
  check_integrity();

  // sync to disk.
  DBStatus<uint64_t> lsn = db_->sync();
  EXPECT_TRUE(lsn);
  if (lsn.has_value()) {
    EXPECT_EQ(*lsn, 1);
  }

  // destroy DB.
  db_.reset();

  // restore DB from disk and check integrity.
  DBStatus<void> ret = open_db();
  EXPECT_TRUE(static_cast<bool>(ret));
  check_integrity();
  db_.reset();

  // corrupt the DB.
  ErrnoStatus<File> f = dir_.openat("0.log", O_WRONLY);
  ASSERT_TRUE(static_cast<bool>(f));
  std::string garbage(kKeySize, '0');
  rand_(std::span(garbage));
  ASSERT_TRUE(
      static_cast<bool>(write_full(*f, std::as_bytes(std::span(garbage)))));

  // now the DB should fail when we restore from disk.
  ret = open_db();
  EXPECT_FALSE(static_cast<bool>(ret));
  if (!ret) {
    EXPECT_EQ(ret.error(), DBCode::kCorruption);
  }
}

TEST_F(DatabaseTest, Compact)
{
  insert_n(kInsertions);
  check_integrity();
  for (int i = 0; i < kCompactionIterations; i++) {
    remove_every_n(2);
    insert_n(kInsertions / 2);
    check_integrity();
  }
}

TEST_F(DatabaseTest, Active)
{
  insert_n(kInsertions);
  check_integrity();
  db_->set_active(false);
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)
  db_->set_active(true);
  check_integrity();
  insert_n(kInsertions / 2);
  check_integrity();
}

TEST_F(DatabaseTest, Snapshot)
{
  insert_n(kInsertions);
  remove_every_n(3);

  std::string path = "test-snapshot.snap";

  auto sync_ret = db_->sync_and_snapshot(path, 0);
  EXPECT_TRUE(sync_ret);
  auto &[lsn, snapshot_future] = *sync_ret;
  EXPECT_EQ(lsn, 1);
  DBStatus<void> ret = snapshot_future.get();
  EXPECT_TRUE(ret);
  check_integrity();

  // ensure the checkpoint file exists and has stuff in it
  ErrnoStatus<struct stat> s = dir_.stat(path);
  ASSERT_TRUE(static_cast<bool>(s));
  ASSERT_GT(s.value().st_size, 10000);

  // replace the recovery log with the snapshot to see if it recovers...
  ErrnoStatus<void> r = dir_.rename(path, "0.log");
  ASSERT_TRUE(static_cast<bool>(r));

  // the DB should succeed when we restore from disk.
  ret = open_db();
  EXPECT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

TEST_F(DatabaseTest, SnapshotRepeatable)
{
  using namespace std::chrono_literals;
  // check to make sure that if we take multiple full snapshots with
  // the last one spanning lsns that we can still recover properly.

  insert_n(kInsertions);
  remove_every_n(3);

  std::string path = "test-snapshot-0-1.snap";
  {
    auto sync_ret = db_->sync_and_snapshot(path, 0);
    ASSERT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    ASSERT_EQ(lsn, 1);
    DBStatus<void> ret = snapshot_future.get();
    ASSERT_TRUE(ret);
  }
  check_integrity();

  // ensure the checkpoint file exists
  ErrnoStatus<File> f = dir_.openat(path, O_RDONLY);
  ASSERT_TRUE(static_cast<bool>(f));

  remove_every_n(1);
  insert_n(kInsertions);

  path = "test-snapshot-0-2.snap";
  {
    auto sync_ret = db_->sync_and_snapshot(path, 0);
    ASSERT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    ASSERT_EQ(lsn, 2);
    DBStatus<void> ret = snapshot_future.get();
    ASSERT_TRUE(ret);
  }
  check_integrity();

  // replace the recovery log with the snapshot to see if it recovers...
  ErrnoStatus<void> r = dir_.rename(path, "0.log");
  ASSERT_TRUE(static_cast<bool>(r));

  // the DB should succeed when we restore from disk,
  // but only recover 0.log (i.e. up to but not including lsn 1)
  auto ret = open_db(1);
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

TEST_F(DatabaseTest, SnapshotIncremental)
{
  std::string path0 = "test-snapshot-0-1.snap";
  std::string path1 = "test-snapshot-1-2.snap";

  insert_n(kInsertions);
  remove_every_n(3);

  {
    auto sync_ret = db_->sync_and_snapshot(path0, 0);
    EXPECT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    EXPECT_EQ(lsn, 1);
    DBStatus<void> ret = snapshot_future.get();
    EXPECT_TRUE(ret);
  }

  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)

  check_integrity();

  // ensure the checkpoint file exists
  ErrnoStatus<File> f0 = dir_.openat(path0, O_RDONLY);
  ASSERT_TRUE(static_cast<bool>(f0));

  // snapshot 0-1 okay, add more and sync, do incremental snapshot
  insert_n(kInsertions / 2);
  {
    auto sync_ret = db_->sync_and_snapshot(path1, 1);
    EXPECT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    EXPECT_EQ(lsn, 2);
    DBStatus<void> ret = snapshot_future.get();
    EXPECT_TRUE(ret);
  }
  check_integrity();

  // ensure the checkpoint file exists
  ErrnoStatus<File> f1 = dir_.openat(path1, O_RDONLY);
  ASSERT_TRUE(static_cast<bool>(f1));

  // replace the recovery log with the snapshot to see if it recovers...
  ErrnoStatus<void> r = dir_.rename(path0, "0.log");
  ASSERT_TRUE(static_cast<bool>(r));
  r = dir_.rename(path1, "1.log");
  ASSERT_TRUE(static_cast<bool>(r));

  // the DB should succeed when we restore from disk.
  auto ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));

  check_integrity();
}

// TODO(abelay): disable this test until we can fix OOM.
TEST_F(DatabaseTest, DISABLED_SnapshotOngoingPuts)
{
  auto rand = rand_; // snapshot rng state.
  insert_n(kInsertions);
  check_integrity();

  // Take a snapshot of the oracle.
  std::unordered_map<std::string, std::string> backup(oracle_);
  auto sync_ret = db_->sync_and_snapshot("snapshot.dat", 0);
  EXPECT_TRUE(sync_ret);
  auto &[_, snapshot_future] = *sync_ret;

  rand_ = rand; // restore rng state to try to get overwrites.
  std::jthread bg{[this] { insert_n(kInsertions); }};

  DBStatus<void> r = snapshot_future.get();
  ASSERT_TRUE(r);

  bg.join();
  check_integrity();

  ErrnoStatus<void> rr = dir_.rename("snapshot.dat", "0.log");
  ASSERT_TRUE(static_cast<bool>(rr));

  auto ret = open_db();
  EXPECT_TRUE(static_cast<bool>(ret));

  // Restore oracle to the moment before the snapshot start.
  std::exchange(oracle_, backup);
  check_integrity();
}

TEST_F(DatabaseTest, DISABLED_SnapshotOngoingRemoves)
{
  insert_n(kInsertions);
  check_integrity();

  // Take a snapshot of the oracle.
  std::unordered_map<std::string, std::string> backup{oracle_};
  auto sync_ret = db_->sync_and_snapshot("snapshot.dat", 0);
  EXPECT_TRUE(sync_ret);
  auto &[_, snapshot_future] = *sync_ret;

  std::jthread bg{[this] { remove_every_n(1); }};

  DBStatus<void> r = snapshot_future.get();
  ASSERT_TRUE(r);

  bg.join();
  check_integrity();

  ErrnoStatus<void> rr = dir_.rename("snapshot.dat", "0.log");
  ASSERT_TRUE(static_cast<bool>(rr));

  auto ret = open_db();
  EXPECT_TRUE(static_cast<bool>(ret));

  // Restore oracle to the moment before the snapshot start.
  std::exchange(oracle_, backup);
  check_integrity();
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
