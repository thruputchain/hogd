// io.cc - utilities for I/O performed by the database

#include "hogd/ramdb/io.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/cleanup.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"

namespace hogd {

namespace {

template<typename Result, typename T>
ErrnoStatus<Result>
make_errno_status(T ret)
{
  // first check for error.
  if (ret == -1)
    return Errno{};

  // otherwise return the result type.
  assert(ret >= 0);
  if constexpr (std::is_integral_v<Result>) {
    return static_cast<Result>(ret);
  }
  else if (std::is_void_v<Result>) {
    assert(ret == 0);
    return {};
  }
  else {
    static_assert("unsupported result type");
  }
}

inline ErrnoStatus<int>
sys_open(const std::string &path, int flags)
{
  return make_errno_status<int>(open(path.c_str(), flags));
}

inline ErrnoStatus<int>
sys_open(const std::string &path, int flags, mode_t mode)
{
  return make_errno_status<int>(open(path.c_str(), flags, mode));
}

inline ErrnoStatus<void>
sys_close(int fd)
{
  return make_errno_status<void>(close(fd));
}

inline ErrnoStatus<size_t>
sys_read(int fd, std::span<std::byte> buf)
{
  return make_errno_status<size_t>(read(fd, buf.data(), buf.size()));
}

inline ErrnoStatus<size_t>
sys_write(int fd, std::span<const std::byte> buf)
{
  return make_errno_status<size_t>(write(fd, buf.data(), buf.size()));
}

inline ErrnoStatus<off_t>
sys_lseek(int fd, off_t offset, int whence)
{
  return make_errno_status<off_t>(lseek(fd, offset, whence));
}

inline ErrnoStatus<void>
sys_fsync(int fd)
{
#if defined(__linux__)
  // Linux provides a version of fsync that makes the file's data persistent
  // but doesn't wait to update the file's other metadata (e.g. atime). This is
  // a little faster when it's available, but it might be unreliable on other
  // platforms.
  return make_errno_status<void>(fdatasync(fd));
#else  // defined(__linux__)
  return make_errno_status<void>(fsync(fd));
#endif // defined(__linux__)
}

inline ErrnoStatus<void>
sys_access(const std::string &path, int mode)
{
  return make_errno_status<void>(access(path.c_str(), mode));
}

inline ErrnoStatus<void>
sys_mkdir(const std::string &path, mode_t mode)
{
  return make_errno_status<void>(mkdir(path.c_str(), mode));
}

inline ErrnoStatus<void>
sys_unlink(const std::string &path)
{
  return make_errno_status<void>(unlink(path.c_str()));
}

} // namespace

ErrnoStatus<File>
File::open(const std::string &path, int flags, mode_t mode)
{
  return sys_open(path, flags, mode).transform([](int fd) { return File(fd); });
}

ErrnoStatus<File>
File::open_temporary()
{
#if defined(__linux__)
  // Linux supports a more advanced form of temporary files that are
  // automatically cleaned up when the file descriptor is closed.
  return sys_open("/tmp", O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR)
      .transform([](int fd) { return File(fd); });
#else  // defined(__linux__)
  char tmp[] = "/tmp/ramdb_XXXXXX";
  int fd = mkstemp(tmp);
  if (fd == -1)
    return Errno{};
  return File(fd);
#endif // defined(__linux__)
}

File::~File()
{
  if (fd_ == kInvalidFD)
    return;

  ErrnoStatus<void> ret = sys_close(fd_);
  if (!ret)
    error("could not close fd {}: {}", fd_, ret.error());
}

ErrnoStatus<size_t>
File::read(std::span<std::byte> buf) const
{
  return sys_read(fd_, buf);
}

ErrnoStatus<size_t>
File::write(std::span<const std::byte> buf) const
{
  return sys_write(fd_, buf);
}

ErrnoStatus<off_t>
File::lseek(off_t offset, int whence) const
{
  return sys_lseek(fd_, offset, whence);
}

ErrnoStatus<void>
File::sync() const
{
  return sys_fsync(fd_);
}

ErrnoStatus<Directory>
Directory::open(const std::string &path, bool create, mode_t mode)
{
  // check if the directory exists or create it if requested.
  ErrnoStatus<void> ret = sys_access(path, F_OK);
  if (!ret) {
    if (ret.error() != ENOENT || !create)
      return ret.error();
    ret = sys_mkdir(path, mode);
    if (!ret)
      return ret.error();
  }

  // create and return the Directory object.
  return Directory(path);
}

ErrnoStatus<std::vector<std::string>>
Directory::list() const
{
  DIR *dir = opendir(dir_path_.c_str());
  if (!dir)
    return Errno{};
  Defer d([dir] { closedir(dir); });

  std::vector<std::string> filenames;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr)
    filenames.emplace_back(entry->d_name);

  return filenames;
}

ErrnoStatus<File>
Directory::openat(std::string_view filename, int flags, mode_t mode) const
{
  return sys_open(get_file_path(filename), flags, mode).transform([](int fd) {
    return File(fd);
  });
}

ErrnoStatus<void>
Directory::access(std::string_view filename, int mode) const
{
  return sys_access(get_file_path(filename), mode);
}

ErrnoStatus<void>
Directory::unlink(std::string_view filename) const
{
  return sys_unlink(get_file_path(filename));
}

ErrnoStatus<void>
Directory::sync() const
{
  ErrnoStatus<int> fd = sys_open(dir_path_, O_DIRECTORY | O_RDWR);
  if (!fd)
    return fd.error();
  Defer d([fd = *fd] {
    if (ErrnoStatus<void> ret = sys_close(fd); !ret)
      error("couldn't close() after dir sync: {}", ret.error());
  });

  return sys_fsync(*fd);
}

ErrnoStatus<size_t>
BufferedReader::read_slowpath(std::span<std::byte> dst)
{
  // try to read from the buffer first
  size_t n = read_from_buffer(dst);
  if (n == dst.size())
    return n;

  // optimization: can read to dst directly if large enough
  dst = dst.subspan(n);
  if (dst.size() >= buf_.size()) {
    ErrnoStatus<size_t> ret = in_.read(dst);
    if (!ret)
      return ret.error();
    return n + *ret;
  }

  // otherwise, refill the buffer and copy what's still needed from it
  assert(pos_.empty());
  ErrnoStatus<void> ret = refill();
  if (!ret)
    return ret.error();
  return n + read_from_buffer(dst);
}

inline size_t
BufferedReader::read_from_buffer(std::span<std::byte> dst)
{
  size_t n = std::min(dst.size(), pos_.size());
  if (n == 0)
    return 0;
  std::copy_n(pos_.begin(), n, dst.begin());
  pos_ = pos_.subspan(n);
  return n;
}

inline ErrnoStatus<void>
BufferedReader::refill()
{
  ErrnoStatus<size_t> ret = in_.read(buf_);
  if (!ret)
    return ret.error();
  pos_ = std::span(buf_.begin(), *ret);
  return {};
}

BufferedWriter::~BufferedWriter()
{
  ErrnoStatus<void> ret = flush();
  if (!ret)
    error("could not flush write buffer: {}", ret.error());
}

ErrnoStatus<size_t>
BufferedWriter::write_slowpath(std::span<const std::byte> src)
{
  size_t bytes_written = 0;

  while (!src.empty()) {
    // optimization: can write src directly if large enough and buffer is empty
    if (buf_.empty() && src.size() >= buf_.capacity()) {
      ErrnoStatus<size_t> ret = out_.write(src);
      if (!ret)
        return ret.error();
      bytes_written += *ret;
      break;
    }

    // append to the buffer
    size_t n = std::min(src.size(), buf_.capacity() - buf_.size());
    std::copy_n(src.begin(), n, std::back_inserter(buf_));
    bytes_written += n;
    src = src.subspan(n);

    // if the buffer is full, flush it to the file
    if (buf_.size() == buf_.capacity()) {
      ErrnoStatus<void> ret = flush();
      if (!ret)
        return ret.error();
    }
  }

  return bytes_written;
}

ErrnoStatus<void>
BufferedWriter::flush()
{
  ErrnoStatus<void> ret = write_full(out_, buf_);
  buf_.clear();
  if (!ret)
    return ret.error();
  return {};
}

ErrnoStatus<void>
BufferedWriter::flush_and_sync()
{
  if (ErrnoStatus<void> ret = flush(); !ret)
    return ret.error();
  return out_.sync();
}

} // namespace hogd
