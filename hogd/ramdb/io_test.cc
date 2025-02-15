#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/ramdb/io.h"
#include "hogd/ramdb/random_bytes.h"

namespace hogd {

namespace {

// the total size of random data to use for testing
constexpr size_t kDataSize = 128_KiB;
// the size of buffering for reads and writes
constexpr size_t kBufferSize = 4_KiB;

} // namespace

class IOTest : public testing::Test {
protected:
  IOTest() : buf_(kDataSize)
  {
    ErrnoStatus<File> ret = File::open_temporary();
    EXPECT_TRUE(ret);
    f_ = std::move(*ret);
    RandomBytes rand;
    rand(std::span(buf_));
  }

  File f_;
  std::vector<std::byte> buf_;
};

TEST_F(IOTest, FullWriteRead)
{
  // perform the write
  ErrnoStatus<void> ret = write_full(f_, std::span(buf_));
  EXPECT_TRUE(ret);

  // seek to beginning of file
  // NOLINTNEXTLINE(misc-include-cleaner)
  ErrnoStatus<off_t> off = f_.lseek(SEEK_SET, 0);
  EXPECT_TRUE(off);
  EXPECT_EQ(*off, 0);

  // perform the read
  std::vector<std::byte> read_buf(kDataSize);
  ret = read_full(f_, std::span(read_buf));
  EXPECT_TRUE(ret);

  // compare the buffers
  EXPECT_EQ(buf_, read_buf);
}

class IOTestP : public IOTest, public ::testing::WithParamInterface<size_t> {};

TEST_P(IOTestP, BufferedWriteRead)
{
  // the number of bytes to transfer each read or write.
  size_t n = GetParam();

  // perform buffered writes
  BufferedWriter w(f_, kBufferSize);
  auto s = std::span(buf_);
  while (!s.empty()) {
    size_t len = std::min(n, s.size());
    ErrnoStatus<void> ret = write_full(w, {s.begin(), len});
    EXPECT_TRUE(ret);
    s = s.subspan(len);
  }

  // seek to beginning of file
  // NOLINTNEXTLINE(misc-include-cleaner)
  ErrnoStatus<off_t> off = f_.lseek(SEEK_SET, 0);
  EXPECT_TRUE(off);
  EXPECT_EQ(*off, 0);

  // perform buffered reads
  std::vector<std::byte> read_buf(kDataSize);
  BufferedReader r(f_, kBufferSize);
  s = std::span(read_buf);
  while (!s.empty()) {
    size_t len = std::min(n, s.size());
    ErrnoStatus<void> ret = read_full(r, {s.begin(), len});
    EXPECT_TRUE(ret);
    s = s.subspan(len);
  }

  // compare the buffers
  EXPECT_EQ(buf_, read_buf);
}

INSTANTIATE_TEST_SUITE_P(IOTests, IOTestP,
                         ::testing::Values(3, 5, 7, 11, 4096, kDataSize));

TEST(DirectoryTest, BasicUsage)
{
  // test opening a directory.
  ErrnoStatus<Directory> dir = Directory::open("/tmp/hogd_io_test");
  EXPECT_TRUE(dir);

  // make sure it's empty.
  ErrnoStatus<std::vector<std::string>> list = dir->list();
  EXPECT_TRUE(list);
  for (std::string filename : *list) {
    EXPECT_TRUE(dir->unlink(filename));
  }

  // create a file.
  ErrnoStatus<File> f = dir->openat("test", O_RDWR | O_CREAT);
  EXPECT_TRUE(f);

  // check that the file appears.
  list = dir->list();
  EXPECT_TRUE(list);
  EXPECT_EQ(list->size(), 1);
  EXPECT_EQ(list->at(0), "test");

  // unlink the file.
  EXPECT_TRUE(dir->unlink(list->at(0)));
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
