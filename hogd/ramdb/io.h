// io.h - utilities for I/O performed by the database

#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/status.h"

namespace hogd {

// kDefaultBufferSize is the default buffer size to use with BufferedReader and
// BufferedWriter.
inline constexpr size_t kDefaultBufferSize = kPageSize * 16;

// default mode used for all files and directories.
inline constexpr mode_t kDefaultMode = (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

// used to track uninitialized File.
inline constexpr int kInvalidFD = -1;

// File wraps a UNIX file.
class File {
public:
  // open opens a file at the specified path.
  static ErrnoStatus<File> open(const std::string &path, int flags,
                                mode_t mode = kDefaultMode);
  // open_temporary creates and opens a temporary file. Writes will be lost
  // after it is closed. This is mostly useful for testing.
  static ErrnoStatus<File> open_temporary();

  File() = default;
  explicit File(int fd) : fd_(fd) {}
  ~File();

  // Enable move; disable copy.
  File(File &&f) noexcept : fd_{std::exchange(f.fd_, kInvalidFD)} {}
  File &operator=(File &&f) noexcept
  {
    fd_ = std::exchange(f.fd_, kInvalidFD);
    return *this;
  }
  File(const File &f) = delete;
  File &operator=(const File &f) = delete;

  // read() system call. Returns the number of bytes read if successful, which
  // could be 0 if EOF.
  ErrnoStatus<size_t> read(std::span<std::byte> buf) const;
  // write() system call. Returns the number of bytes written if successful.
  ErrnoStatus<size_t> write(std::span<const std::byte> buf) const;
  // lseek() system call. Returns the resulting offset as measured from the
  // beginning of the file.
  ErrnoStatus<off_t> lseek(off_t offset, int whence) const;
  // sync flushes all modified file data to the storage device.
  ErrnoStatus<void> sync() const;

private:
  int fd_{kInvalidFD};
};

// has_extension returns true if the filename has the extension ext.
inline bool
has_extension(std::string_view filename, std::string_view ext)
{
  size_t pos = filename.find_last_of('.');
  return pos != std::string_view::npos && filename.substr(pos + 1) == ext;
}

// Directory wraps a UNIX directory.
//
// Linux provides a much better interface based on openat() but it's not
// portable. Instead, we use portable system calls for now, which require us to
// retain the path to the directory and append it as needed to perform each
// operation.
class Directory {
public:
  // open checks if the directory exists and creates it if requested.
  static ErrnoStatus<Directory> open(const std::string &path,
                                     bool create = true, mode_t mode = S_IRWXU);
  // open_temporary creates a temporary directory. Useful for testing.
  static ErrnoStatus<Directory> open_temporary();

  Directory() = default;
  explicit Directory(std::string_view path) : dir_path_(path) {}
  ~Directory() = default;

  // Enable move and copy.
  Directory(Directory &&dir) noexcept : dir_path_(std::move(dir.dir_path_)) {}
  Directory &operator=(Directory &&dir) noexcept
  {
    dir_path_ = std::move(dir.dir_path_);
    return *this;
  }
  Directory(const Directory &dir) = default;
  Directory &operator=(const Directory &dir) = default;

  std::string get_path() const { return dir_path_; }

  // list returns a vector of all the filenames in the directory.
  ErrnoStatus<std::vector<std::string>> list() const;
  // openat opens a file relative to the directory's path.
  ErrnoStatus<File> openat(std::string_view filename, int flags,
                           mode_t mode = kDefaultMode) const;
  // access checks whether a file can exist (and optionally its permissions).
  ErrnoStatus<void> access(std::string_view filename, int mode = F_OK) const;
  // stat returns a file's stat data.
  ErrnoStatus<struct stat> stat(std::string_view filename) const;
  // unlink deletes a filename in this directory.
  ErrnoStatus<void> unlink(std::string_view filename) const;
  // unlink_all deletes all files in this directory.
  ErrnoStatus<void> unlink_all() const;
  // sync flushes all modified directory data to the storage device.
  ErrnoStatus<void> sync() const;

private:
  std::string dir_path_;

  // get_file_path returns a path to a file inside this directory.
  std::string get_file_path(std::string_view filename) const
  {
    return std::string(dir_path_).append("/").append(filename);
  }
};

// BufferedReader provides buffering when reading from a File. This helps to
// reduce kernel crossing overheads.
class BufferedReader {
public:
  BufferedReader(File &in, size_t buf_len = kDefaultBufferSize) : in_(&in)
  {
    buf_.resize(buf_len);
  }
  ~BufferedReader() = default;

  // Allow move, disable copy.
  BufferedReader(BufferedReader &&reader) noexcept
    : in_(reader.in_), buf_(std::move(reader.buf_)),
      pos_(std::exchange(reader.pos_, std::span<const std::byte>()))
  {}
  BufferedReader &operator=(BufferedReader &&reader) noexcept
  {
    in_ = reader.in_;
    buf_ = std::move(reader.buf_);
    pos_ = std::exchange(reader.pos_, std::span<const std::byte>());
    return *this;
  }
  BufferedReader(const BufferedReader &reader) = delete;
  BufferedReader &operator=(const BufferedReader &reader) = delete;

  // read performs buffered reads of a file. Returns the number of bytes read
  // if successful, which could be less than the number requested because only
  // one read will be issued to the underlying file.
  template<size_t N = std::dynamic_extent>
  ErrnoStatus<size_t> read(std::span<std::byte, N> dst)
  {
    // fast path: copy from buffer.
    if (dst.size() < pos_.size()) {
      std::copy_n(pos_.begin(), dst.size(), dst.begin());
      pos_ = pos_.subspan(dst.size());
      return dst.size();
    }

    // slow path: handle direct reads and buffer refill.
    return read_slowpath(dst);
  }

  // read_string returns the string of the bytes before delim or EOF.
  ErrnoStatus<std::string> read_string(char delim);
  // readln returns the string of the bytes before a new line or EOF.
  ErrnoStatus<std::string> readln() { return read_string('\n'); }

private:
  // the unbuffered file to read from.
  File *in_;
  // the buffer.
  std::vector<std::byte> buf_;
  // a span within buf_; contains the bytes that have yet to be read.
  std::span<const std::byte> pos_;

  // read_slowpath is the slowpath for read().
  ErrnoStatus<size_t> read_slowpath(std::span<std::byte> dst);
  // read_from_buffer reads from the currently buffered data.
  size_t read_from_buffer(std::span<std::byte> dst);
  // refill performs a read on the file to refill the buffered data.
  ErrnoStatus<void> refill();

  // peak_delim scans the buffer until it finds delim or reaches the end. It
  // returns the number of bytes scanned including delim if delim was found. It
  // does not read the buffer (only peeks at it).
  std::optional<size_t> peek_delim(char delim) const;
};

// BufferedWriter provides buffering when writing to a File. This helps to
// reduce kernel crossing overheads.
class BufferedWriter {
public:
  BufferedWriter(File &out, size_t buf_len = kDefaultBufferSize) : out_(&out)
  {
    buf_.reserve(buf_len);
  }
  ~BufferedWriter();

  // Allow move, disable copy.
  BufferedWriter(BufferedWriter &&writer) noexcept
    : out_(writer.out_), buf_(std::move(writer.buf_))
  {
    writer.buf_.clear();
  }
  BufferedWriter &operator=(BufferedWriter &&writer) noexcept
  {
    out_ = writer.out_;
    buf_ = std::move(writer.buf_);
    writer.buf_.clear();
    return *this;
  }
  BufferedWriter(const BufferedWriter &writer) = delete;
  BufferedWriter &operator=(const BufferedWriter &writer) = delete;

  // empty returns true if no bytes are buffered.
  bool empty() const { return buf_.empty(); }

  // write performs buffered writes to a file. Returns the number of bytes
  // written if successful, which could be less than the number requested
  // because typically only one write is issued to the underlying file.
  template<size_t N = std::dynamic_extent>
  ErrnoStatus<size_t> write(std::span<const std::byte, N> src)
  {
    // fast path: copy to buffer.
    if (src.size() < buf_.capacity() - buf_.size()) {
      std::copy(src.begin(), src.end(), std::back_inserter(buf_));
      return src.size();
    }

    // slow path: handle direct writes and buffer flushes.
    return write_slowpath(src);
  }

  // flush fully writes all buffered bytes to the file.
  ErrnoStatus<void> flush();
  // flush_and_sync fully writes all buffered bytes to the file and syncs them
  // to disk before returning.
  ErrnoStatus<void> flush_and_sync();

private:
  // the unbuffered file to write to.
  File *out_;
  // the buffer.
  std::vector<std::byte> buf_;

  // write_slowpath is the slowpath for write().
  ErrnoStatus<size_t> write_slowpath(std::span<const std::byte> src);
};

// read_full reads enough bytes to fill buf all the way, blocking if necessary.
// ENODATA is returned if EOF is reached before buf is filled.
template<typename T>
ErrnoStatus<void>
read_full(T &t, std::span<std::byte> buf)
{
  size_t n = 0;
  while (n < buf.size()) {
    ErrnoStatus<size_t> ret = t.read(buf.subspan(n));
    if (!ret)
      return ret.error();
    if (*ret == 0)
      return Errno(ENODATA);
    n += *ret;
  }
  assert(n == buf.size());
  return {};
}

// write_full writes all the bytes in buf, blocking if necessary.
template<typename T>
ErrnoStatus<void>
write_full(T &t, std::span<const std::byte> buf)
{
  size_t n = 0;
  while (n < buf.size()) {
    ErrnoStatus<size_t> ret = t.write(buf.subspan(n));
    if (!ret)
      return ret.error();
    n += *ret;
  }
  assert(n == buf.size());
  return {};
}

} // namespace hogd
