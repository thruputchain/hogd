#include "hogd/ramdb/disk_reader.h"

#include <fcntl.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/loglet.h"

namespace hogd {

namespace {

// LogletBuffer buffers item changes before transferring to memory. This is
// required because we must validate the checksum of all changes in each LSN
// before applying them.
class LogletBuffer {
public:
  LogletBuffer(LogletAllocator *allocator) : allocator_(allocator) {}
  ~LogletBuffer() = default;

  // push_item places src in the buffer and reserves space for its KV pair.
  ItemHeader *push_item(ItemHeader *src);

  // take_buffer returns all pending loglets including the head and resets to
  // an empty buffer.
  std::vector<UniqueLoglet> take_buffer();

  // calculate_crc32c returns the CRC32C of the bytes currently in the buffer.
  uint32_t calculate_crc32c();

  // size returns the number of bytes currently in the buffer.
  size_t size() const
  {
    size_t size = static_cast<bool>(head_) ? head_->size() : 0;
    for (const UniqueLoglet &ul : buf_)
      size += ul->size();
    return size;
  }

private:
  LogletAllocator *allocator_;
  std::vector<UniqueLoglet> buf_;
  UniqueLoglet head_;
  uint32_t crc_{0};
};

inline ItemHeader *
LogletBuffer::push_item(ItemHeader *src)
{
  // allocate a new loglet if needed.
  if (!head_ || head_->bytes_left() < src->total_bytes()) [[unlikely]] {
    if (head_) {
      // it's faster to calculate the CRC now because this head loglet is still
      // hot in the cache.
      crc_ = crc32c_append(crc_, head_->get_bytes());
      buf_.emplace_back(std::move(head_));
    }
    head_ = make_unique_loglet(*allocator_, LogletMode::kDiskBuffer);
    if (!head_)
      return nullptr;
  }

  // reserve space and copy onto the head loglet.
  return head_->clone(src);
}

inline std::vector<UniqueLoglet>
LogletBuffer::take_buffer()
{
  crc_ = 0;
  if (head_)
    buf_.emplace_back(std::move(head_));
  return std::exchange(buf_, std::vector<UniqueLoglet>());
}

inline uint32_t
LogletBuffer::calculate_crc32c()
{
  if (!head_)
    return crc_;
  return crc32c_append(crc_, head_->get_bytes());
}

inline bool
crc_valid(uint32_t commit_crc, LogletBuffer &buf, ItemHeader *commit_hdr)
{
  uint32_t crc = crc32c_append(buf.calculate_crc32c(), byte_view(*commit_hdr));
  if (commit_crc != crc) {
    error("CRC check failed! Expected {:x}, got {:x}.", commit_crc, crc);
    return false;
  }
  return true;
}

// SafeItemHeader is a version of ItemHeader that uses only primitive types.
// This is necessary because it would be UB to load an out of range value into
// storage, as it is an enum class in ItemHeader. Since the on-disk log could
// be corrupted, there's a risk this might happen.
struct SafeItemHeader {
  uint8_t storage;
  uint8_t type;
  uint16_t key_len;
  uint32_t value_len;
  uint64_t lsn;
  uint64_t _padding_garbage_next_pointer; // Always 0 in stable copies of item.

  // returns an ItemHeader if it is valid.
  std::optional<ItemHeader> operator()() const
  {
    if (storage > std::to_underlying(ItemStorage::kCommit))
      return std::nullopt;
    if (key_len == 0)
      return std::nullopt;
    if (sizeof(ItemHeader) + key_len + value_len > kBigItemThreshold)
      return std::nullopt;
    if (_padding_garbage_next_pointer != 0) {
      warn("padding in stored item non-zero");
      return std::nullopt;
    }
    ItemHeader hdr(static_cast<ItemStorage>(storage), type, key_len, value_len);
    hdr.lsn = lsn;
    return hdr;
  }
};

static_assert(sizeof(SafeItemHeader) == sizeof(ItemHeader));
static_assert(std::is_trivially_copyable_v<SafeItemHeader>);

// helper class that parses log and snapshot file names
struct FileMetadata {
  static std::optional<FileMetadata> from_path(const Directory &dir,
                                               std::string_view path)
  {
    FileMetadata file{};

    file.path_ = path;
    file.snapshot_file_ = has_extension(path, "snap");

    ErrnoStatus<struct stat> r = dir.stat(file.path_);
    if (!r) {
      info("couldn't stat filename {}; ignoring", file.path_);
      return std::nullopt;
    }
    file.bytes_ = r->st_size;

    if (file.snapshot_file_) {
      std::vector<std::string_view> parts = split(file.path_, '-');
      if (parts.size() < 2) {
        debug("unexpected snapshot filename {}; ignoring", file.path_);
        return std::nullopt;
      }

      auto lsn = parse_integer<uint64_t>(parts[0]);
      if (!lsn) {
        debug("unexpected snapshot filename {}; ignoring", file.path_);
        return std::nullopt;
      }
      file.begin_lsn_ = *lsn;

      lsn = parse_integer<uint64_t>(parts[1]);
      if (!lsn) {
        debug("unexpected snapshot filename {}; ignoring", file.path_);
        return std::nullopt;
      }
      file.end_lsn_exclusive_ = *lsn;

      return file;
    }

    bool log_file = has_extension(path, "log");
    if (!log_file)
      return std::nullopt;

    std::optional<uint64_t> tmp = parse_integer<uint64_t>(file.path_);
    if (!tmp) {
      debug("invalid log file name `{}`", file.path_);
      return std::nullopt;
    }
    file.begin_lsn_ = *tmp;
    file.end_lsn_exclusive_ = std::numeric_limits<uint64_t>::max();

    return file;
  }

  bool operator<(const FileMetadata &b) const
  {
    if (this == std::addressof(b))
      return false;

    // wal files can only be recovered after any snapshot files.
    if (snapshot_file_ && !b.snapshot_file_)
      return true;

    if (!snapshot_file_ && b.snapshot_file_)
      return false;

    if (begin_lsn_ < b.begin_lsn_)
      return true;

    // Prioritize files that cover "longer" lsn ranges.
    return end_lsn_exclusive_ > b.end_lsn_exclusive_;
  }

  std::string path_;
  uint64_t begin_lsn_;
  uint64_t end_lsn_exclusive_;
  bool snapshot_file_;

  size_t bytes_;

private:
  FileMetadata() = default;
};

DBStatus<void>
load_log_file(
    LogletAllocator &allocator, Directory &dir, const FileMetadata &file,
    uint64_t &cur_lsn, std::optional<uint64_t> target_lsn,
    const std::function<DBStatus<void>(std::vector<UniqueLoglet> &&)> &func)
{
  DBStatus<File> f =
      dir.openat(file.path_, O_RDONLY).transform_error(to_db_error);
  if (!f) {
    warn("couldn't open logfile `{}`: {}", file.path_, f.error());
    return f.error();
  }

  BufferedReader r(*f);
  LogletBuffer buf(&allocator);

  while (!target_lsn || cur_lsn < *target_lsn) {
    // read the next ItemHeader from the log file.
    SafeItemHeader tmp;
    ErrnoStatus<void> ret = read_full(r, writable_byte_view(tmp));
    if (!ret) {
      if (ret.error() == ENODATA)
        break;

      debug("error reading log file");
      return to_db_error(ret.error());
    }

    // validate the header.
    ItemHeader hdr;
    if (std::optional<ItemHeader> ret = tmp(); ret) {
      hdr = *ret;
    }
    else {
      debug("error validating item header");
      return DBError(DBCode::kCorruption);
    }

    bool skip =
        // skip records if that lsn has already been recovered
        (hdr.lsn < cur_lsn) ||
        // snapshot files can have items of different lsn mixed up, so skip
        // items that beyond the lsn range of what we are trying to recover.
        (target_lsn && hdr.lsn >= *target_lsn);

    if (skip) {
      size_t to_skip = hdr.key_len + hdr.value_len;
      ErrnoStatus<size_t> ret = r.skip(to_skip);
      if (!ret)
        return {to_db_error(ret.error())};
      // hit eof while trying to skip
      if (*ret < to_skip)
        break;
      continue;
    }

    // parse the header.
    using enum ItemStorage;
    switch (hdr.storage) {
    case kNormal:
    case kTombstone:
    {
      struct ItemHeader *dst = buf.push_item(&hdr);
      if (!dst)
        return DBError(DBCode::kNoMemory);
      ret = read_full(r, dst->get_all());
      if (!ret)
        return to_db_error(ret.error());
      break;
    }

    case kBig:
      panic("not implemented yet");

    case kCommit:
    {
      // validate the commit header.
      uint32_t commit_crc;
      DBStatus<void> ret = read_full(r, writable_byte_view(commit_crc))
                               .transform_error(to_db_error);
      if (!ret)
        return ret.error();
      if (!crc_valid(commit_crc, buf, &hdr)) {
        info("crc check failure checking commit for lsn {}", hdr.lsn);
        return DBError(DBCode::kCorruption);
      }

      // insert the changes into the database.
      // for WAL files this applies just one lsn of items. snapshot
      // files may commit items of many lsns per commit record.
      ret = func(buf.take_buffer());
      if (!ret)
        return ret.error();

      if (!file.snapshot_file_ && cur_lsn != hdr.lsn) {
        error("replaying log file {}, saw commit for lsn {} while expecting "
              "lsn {}",
              file.path_, hdr.lsn, cur_lsn);
        return DBError{DBCode::kCorruption};
      }

      cur_lsn = hdr.lsn + 1;
    }
    }
  }

  return {};
}

DBStatus<std::vector<FileMetadata>>
find_files(const Directory &dir)
{
  DBStatus<std::vector<std::string>> filenames =
      dir.list().transform_error(to_db_error);
  if (!filenames)
    return filenames.error();

  std::vector<FileMetadata> files{};
  for (auto &fn : *filenames) {
    auto file = FileMetadata::from_path(dir, fn);
    if (file)
      files.emplace_back(*file);
  }
  std::ranges::sort(files, &FileMetadata::operator<);

  return std::move(files);
}

std::vector<FileMetadata>
log_files(const std::vector<FileMetadata> &files)
{
  return files | std::views::filter([](auto &f) { return !f.snapshot_file_; }) |
         std::ranges::to<std::vector<FileMetadata>>();
}

std::optional<FileMetadata>
longest_full_snapshot(const std::vector<FileMetadata> &files)
{
  auto candidates = files | std::views::filter([](auto &f) {
                      return f.snapshot_file_ && f.begin_lsn_ == 0;
                    }) |
                    std::ranges::to<std::vector<FileMetadata>>();

  if (candidates.empty())
    return std::nullopt;

  return std::ranges::max(candidates, &FileMetadata::operator<);
}

} // namespace

DBStatus<uint64_t>
DiskReader::load(Directory &dir, std::optional<uint64_t> target_lsn)
{
  uint64_t lsn;
  DBStatus<void> ret = load_database(
      dir,
      [this](std::vector<UniqueLoglet> &&log) {
        return apply_changes(std::move(log));
      },
      target_lsn, &lsn);
  if (!ret && (target_lsn || lsn == 0))
    return ret.error();
  assert(!target_lsn || *target_lsn == lsn);
  return lsn;
}

DBStatus<uint64_t>
DiskReader::verify(Directory &dir)
{
  uint64_t lsn;
  DBStatus<void> ret = load_database(
      dir,
      [](std::vector<UniqueLoglet> && /*log*/) -> DBStatus<void> { return {}; },
      std::nullopt, &lsn);
  if (!ret && lsn == 0)
    return ret.error();
  return lsn;
}

size_t
DiskReader::log_bytes(const Directory &dir)
{
  auto files = find_files(dir);
  if (!files) {
    warn("couldn't find files for cleanup in {}", dir.get_path());
    return 0;
  }

  auto all_log_files = *files | std::views::filter([](auto &f) {
    return !f.snapshot_file_;
  }) | std::ranges::to<std::vector<FileMetadata>>();

  size_t total_log_bytes =
      std::ranges::fold_left(all_log_files |
                                 std::views::transform(&FileMetadata::bytes_),
                             0, std::plus<>());
  return total_log_bytes;
}

void
DiskReader::cleanup(const Directory &dir)
{
  info("cleaning up log files");

  auto files = find_files(dir);
  if (!files) {
    warn("couldn't find files for cleanup in {}", dir.get_path());
    return;
  }

  uint64_t missing_lsn = 0;
  std::unordered_set<std::string> needed{};

  auto snapshot = longest_full_snapshot(*files);
  if (snapshot) {
    needed.emplace(snapshot->path_);
    missing_lsn = snapshot->end_lsn_exclusive_;
  }

  auto all_log_files = log_files(*files);

  std::span needed_wal_files{all_log_files};
  // a log file is not needed if there is a log file with a higher begin lsn
  // that is <= the first missing lsn (the records it contains must all be
  // less than the newer log file).
  while (needed_wal_files.size() > 1 &&
         needed_wal_files[1].begin_lsn_ <= missing_lsn) {
    needed_wal_files = needed_wal_files.subspan(1);
  }

  for (auto &&f :
       needed_wal_files | std::views::transform(&FileMetadata::path_))
    needed.insert(f);

  for (auto &file : *files) {
    if (needed.contains(file.path_))
      continue;
    info("file {} not needed for recovery; cleaning up", file.path_);
    ErrnoStatus<void> r = dir.unlink(file.path_);
    if (!r)
      warn("failed to remove {} during cleanup", file.path_);
  }
}

DBStatus<void>
DiskReader::load_database(
    Directory &dir,
    const std::function<DBStatus<void>(std::vector<UniqueLoglet> &&)> &func,
    std::optional<uint64_t> target_lsn, uint64_t *loaded_lsn)
{
  uint64_t cur_lsn = 0;
  DBStatus<void> ret{};

  DBStatus<std::vector<std::string>> filenames =
      dir.list().transform_error(to_db_error);
  if (!filenames)
    return filenames.error();

  std::vector<FileMetadata> files{};
  for (auto &fn : *filenames) {
    auto file = FileMetadata::from_path(dir, fn);
    if (file)
      files.emplace_back(*file);
  }
  std::ranges::sort(files, &FileMetadata::operator<);

  std::unordered_set<std::string> used_files{};

  for (FileMetadata &file : files) {
    if (target_lsn && cur_lsn == *target_lsn)
      break;

    if (file.snapshot_file_ && file.begin_lsn_ > cur_lsn) {
      warn("missing data detected: gap at LSN {}", cur_lsn);
      ret = DBError(DBCode::kNotFound);
      break;
    }

    info("loading file {}", file.path_);

    uint64_t tmp = cur_lsn;
    ret = load_log_file(allocator_, dir, file, cur_lsn, target_lsn, func);
    if (!ret) {
      warn("couldn't load logfile {} : {}", file.path_, ret.error());
      break;
    }

    // file contained no useful records
    if (tmp == cur_lsn)
      continue;

    used_files.emplace(file.path_);
  }

  // we reached the end; are we still missing data?
  if (ret && target_lsn && cur_lsn != *target_lsn) {
    warn("missing data detected: loaded LSN {} < target LSN {}", cur_lsn,
         *target_lsn);
    ret = DBError(DBCode::kNotFound);
  }

  if (!ret) {
    if (cur_lsn == 0)
      warn("unable to load any LSNs");
    else
      warn("able to load up to LSN {} despite encountering error", cur_lsn);
  }
  else {
    info("initiating cleanup of skipped files");
    for (auto &file : files) {
      if (used_files.contains(file.path_))
        continue;

      info("removing unused file {}", file.path_);
      ErrnoStatus ret = dir.unlink(file.path_);
      if (!ret)
        warn("couldn't remove unused file {}", file.path_);
    }
  }

  if (loaded_lsn)
    *loaded_lsn = cur_lsn;
  return ret;
}

DBStatus<void>
DiskReader::apply_changes(std::vector<UniqueLoglet> &&log)
{
  // apply all the changes for this LSN.
  for (UniqueLoglet &ll : log) {
    for (ItemHeader &hdr : *ll) {
      if (DBStatus<void> ret = map_.apply_change(&hdr); !ret) [[unlikely]] {
        error("couldn't apply change: {}", ret.error());
        return ret.error();
      }
    }
    ll.reset(); // eagerly release memory.
  }

  // expand the map size if needed.
  if (map_.prepare_expand())
    map_.do_expand();

  return {};
}

} // namespace hogd
