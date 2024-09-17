// -*-C++-*-

#pragma once

#include <filesystem>

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
std::expected<SeekBounds, Errno> seek_bounds(BorrowedFd fd);

template<is_bytevec BufT>
std::expected<size_t, Errno>
readall(BorrowedFd fd, BufT *buf)
{
  if (auto sb = seek_bounds(fd))
    return fullread(fd, buf, sb->end - sb->cur);

  size_t nread{};
  for (;;) {
    if (auto r = read(fd, buf, 4096UZ)) {
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
  return readall(fd, &buf).transform([&buf](size_t) { return std::move(buf); });
}

std::expected<void, Errno> fullwrite(BorrowedFd fd, BufView buf);

std::expected<void, Errno> writefile(const std::filesystem::path &p,
                                     BufView buf, bool sync = false,
                                     int mode = 0666);

// Set (on) or clear (!on) the O_NONBLOCK flag on a file descriptor.
void set_nonblock(BorrowedFd fd, bool on = true);

} // namespace x23
