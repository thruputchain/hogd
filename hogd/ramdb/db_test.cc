#include <gtest/gtest.h>

#include <fcntl.h>

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
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

// the number of buckets in log2(n)
constexpr int kBucketOrder = 7; // 128 buckets
// the size of each key to be inserted
constexpr size_t kKeySize = 256;
// the size of each value to be inserted
constexpr size_t kValueSize = 64;
// the number of items to insert
constexpr size_t kInsertions = 32'768;
// the number of threads to use in concurrent experiments
constexpr int kThreads = 4;
// the number of times to force a compaction
constexpr int kCompactionIterations = 8;

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

  DBStatus<void> open_db();
  void insert_many(size_t count, int threads = 1);
  void remove_every_n(int n);
  void check_integrity();

  Directory dir_;
  std::unique_ptr<Database> db_;
  std::unordered_map<std::string, std::string> oracle_;
  RandomBytes rand_;
};

DBStatus<void>
DatabaseTest::open_db()
{
  size_t heap_size =
      Database::estimate_heap_size(kInsertions * 2, kKeySize, kValueSize);
  DBStatus<std::unique_ptr<Database>> ret =
      Database::open(dir_.get_path(), heap_size, kBucketOrder);
  if (!ret)
    return ret.error();
  db_ = std::move(*ret);
  return {};
}

void
DatabaseTest::insert_many(size_t count, int threads)
{
  using KVPair = std::pair<std::string, std::string>;
  std::vector<std::vector<KVPair>> perthread_inserts(threads);

  // generate random data to insert
  for (size_t i = 0; i < count; ++i) {
    std::string key(kKeySize, '0');
    rand_(std::span(key));
    std::string value(kValueSize, '0');
    rand_(std::span(value));
    perthread_inserts[i % threads].emplace_back(
        std::make_pair(std::move(key), std::move(value)));
  }

  // perform concurrent insertions (with multiple threads)
  {
    std::vector<std::jthread> joins;
    joins.reserve(threads);
    for (int i = 0; i < threads; ++i) {
      joins.emplace_back([this, &inserts = perthread_inserts[i]] {
        for (const auto &[key, value] : inserts) {
          DBStatus<void> ret = db_->put(std::as_bytes(std::span(key)), 0,
                                        std::as_bytes(std::span(value)));
          EXPECT_TRUE(ret);
        }
      });
    }
  }

  // update the oracle
  for (int i = 0; i < threads; ++i) {
    for (auto &[key, value] : perthread_inserts[i]) {
      oracle_.emplace(std::move(key), std::move(value));
    }
  }
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

void
DatabaseTest::check_integrity()
{
  for (const auto &[key, value] : oracle_) {
    DBStatus<DBItem> item = db_->get(std::as_bytes(std::span(key)));
    EXPECT_TRUE(item);
    if (item) {
      EXPECT_EQ(item->type, 0);
      EXPECT_TRUE(item->value == std::as_bytes(std::span(value)));
    }
  }
}

TEST_F(DatabaseTest, SimpleInsertions)
{
  insert_many(kInsertions);
  check_integrity();
}

TEST_F(DatabaseTest, ConcurrentInsertions)
{
  insert_many(kInsertions, kThreads);
  check_integrity();
}

TEST_F(DatabaseTest, InsertionsAndRemovals)
{
  insert_many(kInsertions);
  check_integrity();
  remove_every_n(2);
  check_integrity();
}

TEST_F(DatabaseTest, SyncAndLoad)
{
  // make changes to items.
  insert_many(kInsertions, kThreads);
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
  insert_many(kInsertions, kThreads);
  check_integrity();
  for (int i = 0; i < kCompactionIterations; i++) {
    remove_every_n(2);
    insert_many(kInsertions / 2, kThreads);
    check_integrity();
  }
}

TEST_F(DatabaseTest, Active)
{
  insert_many(kInsertions, kThreads);
  check_integrity();
  db_->set_active(false);
  using namespace std::chrono_literals;
  std::this_thread::sleep_for(10ms); // NOLINT(misc-include-cleaner)
  db_->set_active(true);
  check_integrity();
  insert_many(kInsertions / 2, kThreads);
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
