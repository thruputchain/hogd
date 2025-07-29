// io.cc - utilities for I/O performed by the database

#include "hogd/base/io.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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

inline ErrnoStatus<struct stat>
sys_stat(const std::string &path)
{
  struct stat tmp;
  if (stat(path.c_str(), &tmp) == -1)
    return Errno{};
  return tmp;
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

inline ErrnoStatus<void>
sys_rename(const std::string &old_path, const std::string &new_path)
{
  return make_errno_status<void>(rename(old_path.c_str(), new_path.c_str()));
}

inline std::string
get_tmp_dir()
{
  // use TMPDIR environment variable, of /tmp if not found.
  if (char *tmp = getenv("TMPDIR"); tmp != nullptr)
    return std::string(tmp);
  return "/tmp/";
}

} // namespace

ErrnoStatus<File>
File::open(std::string_view path, int flags, mode_t mode)
{
  return sys_open(std::string(path), flags, mode).transform([](int fd) {
    return File(fd);
  });
}

ErrnoStatus<File>
File::open_temporary()
{
  std::string tmpdir = get_tmp_dir();

#if defined(__linux__)
  // Linux supports a more advanced form of temporary files that are
  // automatically cleaned up when the file descriptor is closed. It may not
  // be available on every filesystem.
  if (auto f = sys_open(tmpdir, O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR)
                   .transform([](int fd) { return File(fd); });
      f) {
    return f;
  }
#endif // defined(__linux__)

  std::string tmp = tmpdir + "ramdb_XXXXXX";
  std::vector<char> buf(tmp.begin(), tmp.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data()); // NOLINT(misc-include-cleaner)
  if (fd == -1)
    return Errno{};

  // unlink the file so it is immediately deleted when the fd is closed.
  std::ignore = sys_unlink(tmp);
  return File(fd);
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

ErrnoStatus<void>
File::close()
{
  if (fd_ == kInvalidFD)
    return {};

  ErrnoStatus<void> ret = sys_close(fd_);
  if (!ret) {
    error("could not close fd {}: {}", fd_, ret.error());
    return ret.error();
  }

  fd_ = kInvalidFD;
  return {};
}

ErrnoStatus<Directory>
Directory::open(std::string_view path, bool create, mode_t mode)
{
  std::string tmp(path);

  // check if the directory exists or create it if requested.
  ErrnoStatus<void> ret = sys_access(tmp, F_OK);
  if (!ret) {
    if (ret.error() != ENOENT || !create)
      return ret.error();
    ret = sys_mkdir(tmp, mode);
    if (!ret)
      return ret.error();
  }

  // create and return the Directory object.
  return Directory(std::move(tmp));
}

ErrnoStatus<Directory>
Directory::open_temporary()
{
  std::string tmp = get_tmp_dir() + "ramdb_XXXXXX";
  std::vector<char> buf(tmp.begin(), tmp.end());
  buf.push_back('\0');
  char *path = mkdtemp(tmp.data()); // NOLINT(misc-include-cleaner)
  if (path == nullptr)
    return Errno{};
  return Directory(std::string(path));
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
  while ((entry = readdir(dir)) != nullptr) {
    if (std::string_view n(entry->d_name); n == "." || n == "..")
      continue;
    filenames.emplace_back(entry->d_name);
  }

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

ErrnoStatus<struct stat>
Directory::stat(std::string_view filename) const
{
  return sys_stat(get_file_path(filename));
}

ErrnoStatus<void>
Directory::unlink(std::string_view filename) const
{
  return sys_unlink(get_file_path(filename));
}

ErrnoStatus<void>
Directory::unlink_all() const
{
  ErrnoStatus<std::vector<std::string>> files = list();
  if (!files)
    return files.error();

  for (std::string_view file : *files) {
    ErrnoStatus<void> ret = unlink(file);
    if (!ret)
      return ret.error();
  }

  return {};
}

ErrnoStatus<void>
Directory::rename(std::string_view old_path, std::string_view new_path) const
{
  return sys_rename(get_file_path(old_path), get_file_path(new_path));
}

ErrnoStatus<void>
Directory::sync() const
{
  ErrnoStatus<int> fd = sys_open(dir_path_, O_DIRECTORY | O_RDONLY);
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
    ErrnoStatus<size_t> ret = in_->read(dst);
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

ErrnoStatus<std::string>
BufferedReader::read_string(char delim)
{
  std::string buf;
  auto append = [&buf](std::span<const std::byte> str) {
    buf.append({reinterpret_cast<const char *>(str.data()), str.size()});
  };

  while (true) {
    // refill the buffer if it is empty
    if (pos_.empty()) {
      ErrnoStatus<void> ret = refill();
      if (!ret)
        return ret.error();
    }

    // if the buffer is still empty, we reached EOF
    if (pos_.empty())
      break;

    // peek to see if the buffer contains the deliminator
    std::optional<size_t> delim_pos = peek_delim(delim);
    if (delim_pos) {
      append(pos_.first(*delim_pos - 1));
      pos_ = pos_.subspan(*delim_pos);
      break;
    }

    // otherwise empty out the current buffer and continue.
    append(std::exchange(pos_, std::span<std::byte>()));
  }

  return buf;
}

ErrnoStatus<size_t>
BufferedReader::skip(size_t n)
{
  size_t left_to_skip = n;
  while (true) {
    size_t s = std::min(left_to_skip, pos_.size());
    pos_ = pos_.subspan(s);
    left_to_skip -= s;

    if (left_to_skip == 0)
      return n;

    ErrnoStatus<void> ret = refill();
    if (!ret)
      return ret.error();

    if (pos_.empty())
      return n - left_to_skip;
  }
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
  ErrnoStatus<size_t> ret = in_->read(buf_);
  if (!ret)
    return ret.error();
  pos_ = std::span(buf_.begin(), *ret);
  return {};
}

inline std::optional<size_t>
BufferedReader::peek_delim(char delim) const
{
  assert(!pos_.empty());

  for (auto it = pos_.begin(); it != pos_.end(); ++it) {
    if (static_cast<char>(*it) == delim)
      return std::distance(pos_.begin(), it) + 1;
  }

  return std::nullopt;
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
      ErrnoStatus<size_t> ret = out_->write(src);
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
  Defer d([this] { buf_.clear(); });
  return write_full(*out_, buf_);
}

ErrnoStatus<void>
BufferedWriter::flush_and_sync()
{
  return flush().and_then([this] { return out_->sync(); });
}

ErrnoStatus<std::string>
read_sys_file(const Directory &dir, std::string_view filename)
{
  return dir.openat(filename, O_RDONLY).and_then([](File &&f) {
    return BufferedReader(f).readln();
  });
}

ErrnoStatus<std::string>
read_sys_file(std::string_view path)
{
  return File::open(std::string(path), O_RDONLY).and_then([](File &&f) {
    return BufferedReader(f).readln();
  });
}

} // namespace hogd
