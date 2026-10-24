#include <x23/fileio.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <sys/socket.h>
#include <sys/uio.h>

#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/vec.h>

namespace x23 {

std::expected<SeekBounds, Errno>
seek_bounds(BorrowedFd fd)
{
  off_t cur = lseek(fd.get(), 0, SEEK_CUR);
  if (cur == -1)
    return Errno{};
  off_t end = lseek(fd.get(), 0, SEEK_END);
  if (lseek(fd.get(), cur, SEEK_SET) == -1 || end == -1)
    return Errno{};
  return SeekBounds{.cur = cur, .end = end};
}

std::expected<void, Errno>
fullwrite(BorrowedFd fd, BufView buf)
{
  for (;;) {
    ssize_t n = ::write(fd.get(), buf.data(), buf.size());
    if (n < 0)
      return Errno{};
    else if (size_t(n) == buf.size())
      return {};
    buf = buf.subspan(n);
  }
}

std::expected<void, Errno>
fullwritev(BorrowedFd fd, std::span<iovec> iov)
{
  for (;;) {
    while (!iov.empty() && iov.front().iov_len == 0)
      iov = iov.subspan(1);
    if (iov.empty())
      return {};
    ssize_t n =
        ::writev(fd.get(), iov.data(),
                 static_cast<int>(std::min<size_t>(iov.size(), IOV_MAX)));
    if (n < 0)
      return Errno{};
    auto done = static_cast<size_t>(n);
    while (done >= iov.front().iov_len) {
      done -= iov.front().iov_len;
      iov = iov.subspan(1);
      if (iov.empty())
        return {};
    }
    iov.front().iov_base = static_cast<char *>(iov.front().iov_base) + done;
    iov.front().iov_len -= done;
  }
}

std::expected<void, Errno>
writefile(const std::filesystem::path &p, BufView buf, bool sync, int mode)
{
  UniqueFd fd(open(p.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode));
  if (!fd)
    return Errno{};
  auto ret = fullwrite(fd, buf);
  if (!ret) {
    return ret;
  }
  if (sync) {
    if (fsync(fd.get()) == -1)
      ret = Errno{};
  }
  return ret;
}
} // namespace x23
