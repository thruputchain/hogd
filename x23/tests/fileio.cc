#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
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
  EXPECT_EQ(st.st_mode & 0077, mode_t{0});

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
  EXPECT_EQ(*result, 4UZ);
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
  EXPECT_EQ(*result, 2UZ);
  EXPECT_EQ(std::string_view(buf), "prefix:ab");
  result = read(fd, &buf, 8);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 1UZ);
  EXPECT_EQ(std::string_view(buf), "prefix:abc");
  result = read(fd, &buf, 8);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 0UZ);
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
  EXPECT_EQ(*result, 3UZ);
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
  EXPECT_EQ(*result, 3UZ);
  EXPECT_EQ(std::string_view(buf), "abc");
  result = fullread(fd, &buf, 0);
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 0UZ);
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
  EXPECT_EQ(*result, 9UZ);
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

// More pieces than one writev() takes, with empty ones among them, land in
// order.
TEST_F(FileIO, FullWritevManyPieces)
{
  std::vector<std::string> pieces;
  std::string expected;
  for (int i = 0; i < 2 * IOV_MAX + 3; i++) {
    pieces.push_back(i % 7 == 0 ? std::string() : std::to_string(i) + ",");
    expected += pieces.back();
  }
  std::vector<iovec> iov;
  for (auto &piece : pieces) {
    iov.push_back({piece.data(), piece.size()});
  }
  {
    UniqueFd fd(open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666));
    ASSERT_TRUE(fd);
    ASSERT_TRUE(fullwritev(fd, iov));
  }
  auto contents = readfile(path_);
  ASSERT_TRUE(contents);
  EXPECT_EQ(std::string_view(*contents), expected);
}

// A file size limit makes writev() stop short at a chosen offset, then fail
// with EFBIG. What reached the file, and the entry left at the front of what
// remains, must let a second call finish the write exactly.
TEST_F(FileIO, FullWritevShortWrite)
{
  const std::vector<std::string> pieces{"abc", "",   "defgh", "",
                                        "",    "ij", "klmnop"};
  std::string expected;
  for (const auto &piece : pieces) {
    expected += piece;
  }
  struct Case {
    rlim_t limit;     // where the short write stops
    size_t resume;    // the entry to resume from
    size_t remaining; // how much of that entry is left
  };
  // Inside "defgh", at the boundary after it, and inside "klmnop".
  for (auto [limit, resume, remaining] :
       {Case{5, 2, 3}, Case{8, 3, 0}, Case{12, 6, 4}}) {
    SCOPED_TRACE(limit);
    std::vector<std::string> copy = pieces;
    std::vector<iovec> iov;
    for (auto &piece : copy) {
      iov.push_back({piece.data(), piece.size()});
    }
    UniqueFd fd(open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666));
    ASSERT_TRUE(fd);

    rlimit old{};
    ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &old), 0);
    auto *old_handler = std::signal(SIGXFSZ, SIG_IGN);
    rlimit low = old;
    low.rlim_cur = limit;
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &low), 0);
    auto written = fullwritev(fd, iov);
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &old), 0);
    std::signal(SIGXFSZ, old_handler);

    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().raw_code(), EFBIG);
    auto contents = readfile(path_);
    ASSERT_TRUE(contents);
    EXPECT_EQ(std::string_view(*contents), expected.substr(0, limit));
    if (remaining > 0) {
      EXPECT_EQ(iov[resume].iov_base,
                copy[resume].data() + copy[resume].size() - remaining);
      EXPECT_EQ(iov[resume].iov_len, remaining);
    }

    ASSERT_TRUE(fullwritev(fd, std::span(iov).subspan(resume)));
    contents = readfile(path_);
    ASSERT_TRUE(contents);
    EXPECT_EQ(std::string_view(*contents), expected);
  }
}

TEST_F(FileIO, FullWritevNothing)
{
  UniqueFd fd(open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666));
  ASSERT_TRUE(fd);
  std::string empty;
  std::vector<iovec> iov{{empty.data(), 0}, {empty.data(), 0}};
  EXPECT_TRUE(fullwritev(fd, iov));
  EXPECT_TRUE(fullwritev(fd, {}));
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
  std::string data = "data";
  std::vector<iovec> iov{{data.data(), data.size()}};
  written = fullwritev(fd, iov);
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
  EXPECT_EQ(*result, 0UZ);
  EXPECT_EQ(std::string_view(buf), "unchanged");
  EXPECT_EQ(lseek(fd.get(), 0, SEEK_CUR), 10);
}
