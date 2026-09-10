#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "x23/fileio.h"

using namespace x23;

namespace {

class FileIO : public ::testing::Test {
protected:
  std::filesystem::path dir_;
  std::filesystem::path path_;

  void SetUp() override
  {
    // CTest's working directory is in the build tree, on local storage.
    char name[] = "fileio-XXXXXX";
    ASSERT_NE(mkdtemp(name), nullptr);
    dir_ = name;
    path_ = dir_ / "data";
  }

  void TearDown() override
  {
    if (!dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
      EXPECT_FALSE(ec) << ec.message();
    }
  }
};

} // namespace

TEST_F(FileIO, WriteReadAndTruncate)
{
  std::string data("hello\0world", 11);
  ASSERT_TRUE(writefile(path_, data, true, 0600));
  auto result = readfile(path_);
  ASSERT_TRUE(result);
  EXPECT_EQ(std::string_view(*result), data);

  struct stat st;
  ASSERT_EQ(stat(path_.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0077, 0);

  ASSERT_TRUE(writefile(path_, std::string_view("short")));
  result = readfile(path_);
  ASSERT_TRUE(result);
  EXPECT_EQ(std::string_view(*result), "short");

  ASSERT_TRUE(writefile(path_, BufView{}));
  result = readfile(path_);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->empty());
}

TEST_F(FileIO, SeekBoundsPreservesOffset)
{
  ASSERT_TRUE(writefile(path_, std::string_view("abcdef")));
  UniqueFd fd(open(path_.c_str(), O_RDONLY));
  ASSERT_TRUE(fd);
  ASSERT_EQ(lseek(fd.get(), 2, SEEK_SET), 2);
  auto bounds = seek_bounds(fd);
  ASSERT_TRUE(bounds);
  EXPECT_EQ(bounds->cur, 2);
  EXPECT_EQ(bounds->end, 6);
  EXPECT_EQ(lseek(fd.get(), 0, SEEK_CUR), 2);

  Str buf = "prefix:";
  auto result = readall(fd, &buf);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 4);
  EXPECT_EQ(std::string_view(buf), "prefix:cdef");
}

TEST_F(FileIO, ReadAppendsAndStopsAtEof)
{
  ASSERT_TRUE(writefile(path_, std::string_view("abc")));
  UniqueFd fd(open(path_.c_str(), O_RDONLY));
  ASSERT_TRUE(fd);
  Str buf = "prefix:";
  auto result = read(fd, &buf, 2);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 2);
  EXPECT_EQ(std::string_view(buf), "prefix:ab");
  result = read(fd, &buf, 8);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 1);
  EXPECT_EQ(std::string_view(buf), "prefix:abc");
  result = read(fd, &buf, 8);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 0);
  EXPECT_EQ(std::string_view(buf), "prefix:abc");
}

TEST_F(FileIO, FullReadPrematureEof)
{
  ASSERT_TRUE(writefile(path_, std::string_view("abc")));
  UniqueFd fd(open(path_.c_str(), O_RDONLY));
  ASSERT_TRUE(fd);
  Str buf;
  auto result = fullread(fd, &buf, 5);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().raw_code(), ECONNRESET);
  EXPECT_EQ(std::string_view(buf), "abc");

  ASSERT_EQ(lseek(fd.get(), 0, SEEK_SET), 0);
  buf.clear();
  result = fullread(fd, &buf, 5, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 3);
  EXPECT_EQ(std::string_view(buf), "abc");
}

TEST_F(FileIO, FullReadExactAndZeroLength)
{
  ASSERT_TRUE(writefile(path_, std::string_view("abc")));
  UniqueFd fd(open(path_.c_str(), O_RDONLY));
  ASSERT_TRUE(fd);
  Str buf;
  auto result = fullread(fd, &buf, 3);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 3);
  EXPECT_EQ(std::string_view(buf), "abc");
  result = fullread(fd, &buf, 0);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 0);
  EXPECT_EQ(std::string_view(buf), "abc");
}

TEST_F(FileIO, ReadAllUnseekable)
{
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  UniqueFd reader(fds[0]), writer(fds[1]);
  ASSERT_TRUE(fullwrite(writer, std::string_view("pipe data")));
  writer.reset();

  auto bounds = seek_bounds(reader);
  ASSERT_FALSE(bounds);
  EXPECT_EQ(bounds.error().raw_code(), ESPIPE);
  Str buf = "prefix:";
  auto result = readall(reader, &buf);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 9);
  EXPECT_EQ(std::string_view(buf), "prefix:pipe data");
}

TEST_F(FileIO, MissingFileAndParent)
{
  auto result = readfile(path_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().raw_code(), ENOENT);
  auto written = writefile(dir_ / "missing" / "data", BufView{});
  ASSERT_FALSE(written);
  EXPECT_EQ(written.error().raw_code(), ENOENT);
}

TEST_F(FileIO, InvalidDescriptorPreservesBuffer)
{
  BorrowedFd fd(-1);
  Str buf = "unchanged";
  auto result = read(fd, &buf, 10);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().raw_code(), EBADF);
  EXPECT_EQ(std::string_view(buf), "unchanged");
  result = fullread(fd, &buf, 10);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().raw_code(), EBADF);
  EXPECT_EQ(std::string_view(buf), "unchanged");
  result = readall(fd, &buf);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().raw_code(), EBADF);
  EXPECT_EQ(std::string_view(buf), "unchanged");
  auto written = fullwrite(fd, std::string_view("data"));
  ASSERT_FALSE(written);
  EXPECT_EQ(written.error().raw_code(), EBADF);
}

TEST_F(FileIO, ReadFilePropagatesReadError)
{
  auto result = readfile(dir_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().raw_code(), EISDIR);
}

TEST_F(FileIO, ReadAllBeyondEof)
{
  ASSERT_TRUE(writefile(path_, std::string_view("abc")));
  UniqueFd fd(open(path_.c_str(), O_RDONLY));
  ASSERT_TRUE(fd);
  ASSERT_EQ(lseek(fd.get(), 10, SEEK_SET), 10);
  Str buf = "unchanged";
  auto result = readall(fd, &buf);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 0);
  EXPECT_EQ(std::string_view(buf), "unchanged");
  EXPECT_EQ(lseek(fd.get(), 0, SEEK_CUR), 10);
}
