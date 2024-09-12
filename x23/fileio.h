// -*-C++-*-

#pragma once

#include <filesystem>
#include <type_traits>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <x23/cleanup.h>
#include <x23/err.h>
#include <x23/vec.h>

namespace x23 {

template<is_bytevec BufT>
std::expected<size_t, Errno>
read(BorrowedFd fd, BufT *buf, size_t len)
{
  size_t old_size = buf->size();
  std::expected<ssize_t, Errno> ret;
  buf->resize_and_overwrite(old_size + len, [fd = fd.get(), len, old_size,
                                             &ret](auto *p, size_t count) {
    if (ssize_t n = ::read(fd, p + old_size, len); n >= 0) {
      ret = n;
      return old_size + n;
    }
    ret = Errno{};
    return old_size;
  });
  return ret;
}

// Keep reading until you have read len bytes.  Returns ECONNRESET on
// a premature EOF.
template<is_bytevec BufT>
std::expected<void, Errno>
fullread(BorrowedFd fd, BufT *buf, size_t len)
{
  size_t nread = 0;
  while (len) {
    auto r = read(fd, buf, len);
    if (!r)
      return r.error();
    if (!*r)
      return Errno{ECONNRESET};
    nread += *r;
    len -= *r;
  }
  return {};
}

struct SeekBounds {
  off_t cur;
  off_t end;
};
inline std::expected<SeekBounds, Errno>
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

template<is_bytevec BufT>
std::expected<size_t, Errno>
readall(BorrowedFd fd, BufT *buf)
{
  if (auto sb = seek_bounds(fd))
    return fullread(fd, buf, sb->end - sb->cur);

  size_t nread{};
  for (;;) {
    if (auto r = read(fd, buf, 4096uz)) {
      if (!*r)
        return nread;
      nread += *r;
    }
    else
      return r.error();
  }
}

template<is_bytevec BufT = Str>
std::expected<Buf, Errno>
readfile(std::filesystem::path p)
{
  UniqueFd fd(open(p.c_str(), O_RDONLY));
  if (!fd)
    return Errno{};
  BufT buf;
  return readall(fd, &buf).transform(
      [&buf](size_t) { return std::move(buf); });
}

inline std::expected<void, Errno>
fullwrite(BorrowedFd fd, BufView buf)
{
  for (;;) {
    ssize_t n = ::write(fd.get(), buf.data(), buf.size());
    if (n <= 0)
      return Errno{};
    else if (size_t(n) == buf.size())
      return {};
    buf = buf.subspan(n);
  }
}

inline std::expected<void, Errno>
writefile(std::filesystem::path p, BufView buf,
	  bool sync = false, int mode = 0666)
{
  UniqueFd fd(open(p.c_str(), O_CREAT|O_TRUNC|O_WRONLY, mode));
  if (!fd)
    return Errno{};
  auto ret = fullwrite(fd, buf);
  if (ret) {
    if (fsync(fd.get()) == -1)
      ret = Errno{};
  }
  return ret;
}

} // namespace x23
