#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/random_bytes.h"
#include "hogd/base/status.h"
#include "hogd/base/time.h"
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
  open_db(uint64_t target_lsn = std::numeric_limits<uint64_t>::max(),
          size_t log_file_size_threshold = kDiskLogFileThreshold);
  void insert_n(size_t count);
  DBError insert_until_failure();
  void remove_every_n(int n);
  void check_integrity();
  void sync_n(uint64_t end_lsn_exclusive, uint64_t &lsn);
  void check_file_exists(std::string_view filename);
  void wait_for_deletion(std::string_view path);

  Directory dir_;
  std::unique_ptr<Database> db_;
  std::unordered_map<std::string, std::string> oracle_;
  RandomBytes rand_;
};

DBStatus<void>
DatabaseTest::open_db(uint64_t target_lsn, size_t log_file_size_threshold)
{
  size_t heap_size =
      Database::estimate_heap_size(kInsertions * 2, kKeySize, kValueSize);
  DBStatus<std::unique_ptr<Database>> ret =
      target_lsn == std::numeric_limits<uint64_t>::max()
          ? Database::open(dir_.get_path(), heap_size, kBucketOrder,
                           log_file_size_threshold)
          : Database::open_lsn(dir_.get_path(), target_lsn, heap_size,
                               kBucketOrder, log_file_size_threshold);
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

void
DatabaseTest::sync_n(uint64_t end_lsn_exclusive, uint64_t &lsn)
{
  while (lsn < end_lsn_exclusive) {
    insert_n(1);
    DBStatus<uint64_t> r = db_->sync();
    ASSERT_TRUE(r);
    ASSERT_EQ(*r, lsn + 1);
    lsn = *r;
  }
  check_integrity();
}

void
DatabaseTest::check_file_exists(std::string_view filename)
{
  ErrnoStatus<File> f = dir_.openat(filename, O_RDONLY);
  if (!f)
    FAIL() << "file does not exist: " << filename;
}

void
DatabaseTest::wait_for_deletion(std::string_view path)
{
  ElapsedTimer timer{};
  constexpr double kTimeout = 2.0;

  while (timer.elapsed() < kTimeout) {
    ErrnoStatus<File> f = dir_.openat(path, O_RDONLY);
    if (!f)
      return;

    using namespace std::chrono_literals;
    std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)
  }

  FAIL() << "file did not get deleted as expected: " << path;
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

  auto sync_ret = db_->sync_and_snapshot(0);
  EXPECT_TRUE(sync_ret);
  auto &[lsn, snapshot_future] = *sync_ret;
  EXPECT_EQ(lsn, 1);
  DBStatus<void> ret = snapshot_future.get();
  EXPECT_TRUE(ret);
  check_integrity();

  // ensure the checkpoint file exists and has stuff in it
  const std::string path = "0-1.snap";
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
  // check to make sure that if we take multiple full snapshots with
  // the last one spanning lsns that we can still recover properly.

  insert_n(kInsertions);
  remove_every_n(3);

  {
    auto sync_ret = db_->sync_and_snapshot(0);
    ASSERT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    ASSERT_EQ(lsn, 1);
    DBStatus<void> ret = snapshot_future.get();
    ASSERT_TRUE(ret);
  }
  check_integrity();
  check_file_exists("0-1.snap");

  remove_every_n(1);
  insert_n(kInsertions);

  {
    auto sync_ret = db_->sync_and_snapshot(0);
    ASSERT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    ASSERT_EQ(lsn, 2);
    DBStatus<void> ret = snapshot_future.get();
    ASSERT_TRUE(ret);
  }
  check_integrity();

  auto _ = dir_.unlink("0.log");
  _ = dir_.unlink("1.log");

  auto ret = open_db(2);
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();

  ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

TEST_F(DatabaseTest, SnapshotIncremental)
{
  insert_n(kInsertions);
  remove_every_n(3);

  {
    auto sync_ret = db_->sync_and_snapshot(0);
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
  check_file_exists("0-1.snap");

  // snapshot 0-1 okay, add more and sync, do incremental snapshot
  insert_n(kInsertions / 2);
  {
    auto sync_ret = db_->sync_and_snapshot(1);
    EXPECT_TRUE(sync_ret);
    auto &[lsn, snapshot_future] = *sync_ret;
    EXPECT_EQ(lsn, 2);
    DBStatus<void> ret = snapshot_future.get();
    EXPECT_TRUE(ret);
  }
  check_integrity();

  // ensure the checkpoint file exists
  check_file_exists("1-2.snap");
  // ensure we can recover from just snapshot files.
  ASSERT_TRUE(dir_.unlink("0.log"));

  // the DB should succeed when we restore from disk.
  auto ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));

  check_integrity();
}

TEST_F(DatabaseTest, SnapshotOngoingPuts)
{
  auto rand = rand_; // snapshot rng state.
  insert_n(kInsertions);
  check_integrity();

  // Take a snapshot of the oracle.
  std::unordered_map<std::string, std::string> backup(oracle_);
  auto sync_ret = db_->sync_and_snapshot(0);
  EXPECT_TRUE(sync_ret);
  auto &[_, snapshot_future] = *sync_ret;

  rand_ = rand; // restore rng state to try to get overwrites.
  std::jthread bg{[this] { insert_n(kInsertions); }};

  DBStatus<void> r = snapshot_future.get();
  ASSERT_TRUE(r);

  bg.join();
  check_integrity();

  EXPECT_TRUE(dir_.unlink("0.log"));

  auto ret = open_db();
  EXPECT_TRUE(static_cast<bool>(ret));

  // Restore oracle to the moment before the snapshot start.
  std::exchange(oracle_, backup);
  check_integrity();
}

TEST_F(DatabaseTest, SnapshotOngoingRemoves)
{
  insert_n(kInsertions);
  check_integrity();

  // Take a snapshot of the oracle.
  std::unordered_map<std::string, std::string> backup{oracle_};
  auto sync_ret = db_->sync_and_snapshot(0);
  EXPECT_TRUE(sync_ret);
  auto &[_, snapshot_future] = *sync_ret;

  std::jthread bg{[this] { remove_every_n(1); }};

  DBStatus<void> r = snapshot_future.get();
  ASSERT_TRUE(r);

  bg.join();
  check_integrity();

  EXPECT_TRUE(dir_.unlink("0.log"));

  auto ret = open_db();
  EXPECT_TRUE(static_cast<bool>(ret));

  // Restore oracle to the moment before the snapshot start.
  std::exchange(oracle_, backup);
  check_integrity();
}

// ensure that recovery uses both snapshots and wal files together to recover.
TEST_F(DatabaseTest, RecoverFromSnapshotAndLog)
{
  uint64_t lsn = 0;

  // wal contains 1 record with lsn 0 and 1 record with lsn 1.
  sync_n(2, lsn);
  check_file_exists("0.log");
  // 1.log doesn't yet exist because sync() doesn't create a new
  // log file unless the log file is over a certain size.

  // add a record in lsn 2.
  insert_n(1);

  // snapshot all three records.
  auto r = db_->sync_and_snapshot(0);
  ASSERT_TRUE(r);
  lsn = r->first;
  ASSERT_EQ(lsn, 3);
  DBStatus<void> ret = r->second.get();
  ASSERT_TRUE(ret);

  // make sure the db wasn't affected.
  check_integrity();

  // add a new record in lsn 3 and sync it.
  insert_n(1);
  sync_n(4, lsn);

  // record and ensure all 4 records are recovered.
  ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

TEST_F(DatabaseTest, RecoverFromIncrementalSnapshots)
{
  uint64_t lsn = 0;

  // wal contains 1 record with lsn 0 and 1 record with lsn 1.
  sync_n(2, lsn);
  check_file_exists("0.log");
  // 1.log doesn't yet exist because sync() doesn't create a new
  // log file unless the log file is over a certain size.

  // snapshot the two records
  auto r = db_->sync_and_snapshot(0);
  ASSERT_TRUE(r);
  lsn = r->first;
  ASSERT_EQ(lsn, 3);
  DBStatus<void> ret = r->second.get();
  ASSERT_TRUE(ret);

  // add a record in lsn 2.
  sync_n(3, lsn);

  // incremental snapshot that includes the new record
  r = db_->sync_and_snapshot(3);
  ASSERT_TRUE(r);
  lsn = r->first;
  ASSERT_EQ(lsn, 4);
  ret = r->second.get();
  ASSERT_TRUE(ret);

  // add a record in lsn 4 that is in WAL only.
  sync_n(5, lsn);

  // make sure the db wasn't affected.
  check_integrity();

  // record and ensure all records are recovered.
  ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

TEST_F(DatabaseTest, RecoverSkipsUncommittedLSNs)
{
  // This tests a specific case. In it, one record is synced and a second
  // record is added, both in the same WAL file. Then, the db is recovered,
  // which should only recover the first record. Then, a new record is
  // added/synced, which should be written to a new WAL file with a higher lsn
  // name. Then, the db is recovered. The db should recover even though there
  // are uncommitted entries in the middle of the log so long as the second
  // file's commit record has the same lsn as the records that were skipped in
  // the first file.

  uint64_t lsn = 0;

  // commit one record in lsn 0 and one in lsn 1
  sync_n(2, lsn);
  // commit one record in lsn 2 and one in lsn 3
  sync_n(3, lsn);
  auto oracle = oracle_;
  sync_n(4, lsn);

  check_file_exists("0.log");
  wait_for_deletion("1.log");
  wait_for_deletion("2.log");
  wait_for_deletion("3.log");

  // remove lsn 3 commit record from end of 0.log to "uncommit" lsn 3.
  {
    ErrnoStatus<File> f = dir_.openat("0.log", O_WRONLY | O_APPEND);
    ASSERT_TRUE(f);
    auto len = f->lseek(0, SEEK_END); // NOLINT(misc-include-cleaner)
    ASSERT_TRUE(len);
    auto r = f->ftruncate(*len - (sizeof(ItemHeader) * 2));
    ASSERT_TRUE(r);
    lsn--;
  }

  DBStatus<void> ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));

  // reset oracle to state at end of lsn 1 commit.
  oracle_ = oracle;
  check_integrity();

  // add 1 record with lsn 3
  sync_n(4, lsn);
  wait_for_deletion("1.log");
  wait_for_deletion("2.log");
  check_file_exists("3.log");

  ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));

  check_integrity();
}

TEST_F(DatabaseTest, DiskCleanup)
{
  constexpr uint64_t kInfTargetLSN = std::numeric_limits<uint64_t>::max();
  constexpr size_t kDiskLogFileSize = 1 * 1024 * 1024; // 1 MB
  constexpr size_t kItemSize = kKeySize + kValueSize;
  constexpr size_t kEntriesPerLogFile = (2 * kDiskLogFileSize) / kItemSize;
  constexpr size_t kBatchSize = 1024;
  constexpr size_t kBatches = kEntriesPerLogFile / kBatchSize;

  // reset db to use much smaller log file sizes so test runs quickly.
  db_.reset();
  ASSERT_TRUE(dir_.unlink_all());
  DBStatus<void> ret = open_db(kInfTargetLSN, kDiskLogFileSize);
  ASSERT_TRUE(ret);

  uint64_t lsn = 0;

  auto fill_file_and_sync = [this, &lsn](size_t n_syncs) {
    for (; lsn < n_syncs; lsn++) {
      // doing in batches controls heap size for the test while growing the log.
      for (size_t b = 0; b < kBatches; b++) {
        insert_n(kBatchSize);
        remove_every_n(1);
      }

      if (lsn > 0)
        check_file_exists(std::format("{}.log", lsn - 1));

      DBStatus<uint64_t> r = db_->sync();
      ASSERT_TRUE(r);
      ASSERT_EQ(*r, lsn + 1);
    }
  };

  // create enough log files that we exceed the cleaning threshold.
  fill_file_and_sync(2);

  check_file_exists("0.log");
  check_file_exists("1.log");

  // manually trigger a snapshot so some of the wal files become redundant.
  auto sync_ret = db_->sync_and_snapshot(0);
  EXPECT_TRUE(sync_ret);
  auto &[new_lsn, snapshot_future] = *sync_ret;
  EXPECT_EQ(new_lsn, 3);
  DBStatus<void> snap_ret = snapshot_future.get();
  EXPECT_TRUE(snap_ret);

  // all but the newest wal file should get cleaned up eventually.
  wait_for_deletion("0.log");
  wait_for_deletion("1.log");
  check_file_exists("2.log");

  ret = open_db(kInfTargetLSN, kDiskLogFileSize);
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

// IncrementalSnapshotAfterReopenRejectsGap
//
// After reopen, sync() returns an LSN that exceeds the snapshot coverage
// (e.g., the prior snapshot covers 0-1 but sync returns 2). Using that LSN
// as begin_lsn for sync_and_snapshot would create a gap in snapshot coverage
// that recovery cannot bridge. sync_and_snapshot must reject this.
TEST_F(DatabaseTest, IncrementalSnapshotAfterReopenRejectsGap)
{
  // Step 1: snapshot the empty database. Snapshot covers LSN 0-1.
  auto snap1 = db_->sync_and_snapshot(0);
  ASSERT_TRUE(snap1.has_value());
  auto &[lsn1, future1] = *snap1;
  ASSERT_TRUE(future1.get().has_value());

  // Step 2: sync + close + reopen.
  auto sync1 = db_->sync();
  ASSERT_TRUE(sync1.has_value());
  uint64_t last_lsn = *sync1; // LSN 2 — beyond snapshot coverage (1).
  db_.reset();
  auto ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();

  // Step 3: insert one item.
  std::string key(kKeySize, 'A');
  std::string value(kValueSize, 'B');
  auto put_ret = db_->put(std::as_bytes(std::span(key)), 0,
                          std::as_bytes(std::span(value)));
  ASSERT_TRUE(put_ret.has_value());
  oracle_[key] = value;

  // Step 4: sync_and_snapshot(last_lsn) should be rejected — last_lsn (2)
  // exceeds the snapshot coverage (1), which would create an unrecoverable gap.
  auto snap2 = db_->sync_and_snapshot(last_lsn);
  ASSERT_FALSE(snap2.has_value())
      << "sync_and_snapshot should reject begin_lsn that creates a gap";

  // Step 5: Using begin_lsn=0 (full snapshot) should succeed.
  auto snap3 = db_->sync_and_snapshot(0);
  ASSERT_TRUE(snap3.has_value());
  auto &[lsn3, future3] = *snap3;
  ASSERT_TRUE(future3.get().has_value());

  // Step 6: sync + close + reopen and verify. Data is preserved.
  auto sync3 = db_->sync();
  ASSERT_TRUE(sync3.has_value());
  db_.reset();
  ret = open_db();
  ASSERT_TRUE(static_cast<bool>(ret));
  check_integrity();
}

// Regression for #1605: the recovery skip path used the unpadded
// key_len + value_len, but the writer pads kNormal/kTombstone records to
// alignof(ItemHeader). After a snapshot covers a misaligned record, recovery
// reads the next header from inside that padding and reports kCorruption.
TEST_F(DatabaseTest, RecoverySkipAlignsPaddedRecord)
{
  constexpr uint64_t kInfTargetLSN = std::numeric_limits<uint64_t>::max();
  constexpr size_t kRolloverAfterFirstRecordBytes = 31;

  db_.reset();
  ASSERT_TRUE(dir_.unlink_all());
  DBStatus<void> ret = open_db(kInfTargetLSN, kRolloverAfterFirstRecordBytes);
  ASSERT_TRUE(ret);

  std::string odd_key = "abcde";
  std::string empty_value;
  // key_len + value_len must be unaligned for the bug to bite.
  ASSERT_NE((odd_key.size() + empty_value.size()) % alignof(ItemHeader), 0U);

  DBStatus<void> put_ret = db_->put(std::as_bytes(std::span(odd_key)), 0,
                                    std::as_bytes(std::span(empty_value)));
  ASSERT_TRUE(put_ret);

  auto snap = db_->sync_and_snapshot(0);
  ASSERT_TRUE(snap);
  ASSERT_EQ(snap->first, 1);
  ASSERT_TRUE(snap->second.get());
  check_file_exists("0-1.snap");
  check_file_exists("0.log");

  std::string next_key = "next-key";
  std::string next_value = "still reachable";
  put_ret = db_->put(std::as_bytes(std::span(next_key)), 0,
                     std::as_bytes(std::span(next_value)));
  ASSERT_TRUE(put_ret);
  DBStatus<uint64_t> lsn = db_->sync();
  ASSERT_TRUE(lsn);
  ASSERT_EQ(*lsn, 2);
  check_file_exists("1.log");

  db_.reset();

  // Pre-fix: 0.log's misaligned record desynced the reader and recovery
  // failed with kCorruption. Post-fix: recovery succeeds and both records
  // are observable.
  ret = open_db(kInfTargetLSN, kRolloverAfterFirstRecordBytes);
  ASSERT_TRUE(static_cast<bool>(ret));

  DBStatus<DBItem> got_odd = db_->get(std::as_bytes(std::span(odd_key)));
  ASSERT_TRUE(got_odd);
  EXPECT_EQ(got_odd->value.size(), 0U);

  DBStatus<DBItem> got_next = db_->get(std::as_bytes(std::span(next_key)));
  ASSERT_TRUE(got_next);
  EXPECT_EQ(to_string(got_next->value), next_value);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv, false);
  if (ret)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
