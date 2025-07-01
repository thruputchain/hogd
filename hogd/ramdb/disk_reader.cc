#include "hogd/ramdb/disk_reader.h"

#include <fcntl.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

// get_log_files scans the directory and returns a vector of the starting LSN
// for each log file that was found.
inline DBStatus<std::vector<uint64_t>>
get_log_files(Directory &dir)
{
  DBStatus<std::vector<std::string>> files =
      dir.list().transform_error(to_db_error);
  if (!files)
    return files.error();

  std::vector<uint64_t> lsns;
  for (std::string_view str : *files) {
    if (!has_extension(str, "log"))
      continue;

    std::optional<uint64_t> tmp = parse_integer<uint64_t>(str);
    if (!tmp) {
      warn("invalid log file name `{}`", str);
      continue;
    }

    lsns.push_back(*tmp);
  }
  std::ranges::sort(lsns);

  return lsns;
}

// lsn_to_filename converts a starting LSN to the corresponding log filename.
inline std::string
lsn_to_filename(uint64_t lsn)
{
  return std::format("{}.log", lsn);
}

} // namespace

DBStatus<uint64_t>
DiskReader::load(Directory &dir, std::optional<uint64_t> target_lsn)
{
  uint64_t lsn;
  DBStatus<void> ret = load_database(
      dir,
      [this](uint64_t lsn, std::vector<UniqueLoglet> &&log) {
        return apply_changes(lsn, std::move(log));
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
      [](uint64_t /*lsn*/,
         std::vector<UniqueLoglet> && /*log*/) -> DBStatus<void> { return {}; },
      std::nullopt, &lsn);
  if (!ret && lsn == 0)
    return ret.error();
  return lsn;
}

DBStatus<void>
DiskReader::load_log_file(
    File &f, uint64_t &cur_lsn, std::optional<uint64_t> target_lsn,
    const std::function<DBStatus<void>(uint64_t lsn,
                                       std::vector<UniqueLoglet> &&)> &func)
{
  BufferedReader r(f);
  LogletBuffer buf(&allocator_);

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
    if (hdr.lsn != cur_lsn) {
      debug("mismatched log file lsn header lsn {}, expected lsn {}", hdr.lsn,
            cur_lsn);
      return DBError(DBCode::kCorruption);
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
      if (!crc_valid(commit_crc, buf, &hdr))
        return DBError(DBCode::kCorruption);

      // insert the changes for this LSN into the database.
      ret = func(cur_lsn, buf.take_buffer());
      if (!ret)
        return ret.error();
      cur_lsn++;
    }
    }
  }

  return {};
}

DBStatus<void>
DiskReader::load_database(
    Directory &dir,
    const std::function<DBStatus<void>(uint64_t lsn,
                                       std::vector<UniqueLoglet> &&)> &func,
    std::optional<uint64_t> target_lsn, uint64_t *loaded_lsn)
{
  uint64_t cur_lsn = 0;
  DBStatus<void> ret{};
  DBStatus<std::vector<uint64_t>> log_lsns = get_log_files(dir);
  if (!log_lsns)
    return log_lsns.error();

  for (uint64_t log_lsn : *log_lsns) {
    if (target_lsn && log_lsn >= *target_lsn) {
      assert(log_lsn == *target_lsn);
      break;
    }

    if (log_lsn != cur_lsn) {
      warn("missing data detected: gap at LSN {}", cur_lsn);
      ret = DBError(DBCode::kNotFound);
      break;
    }

    DBStatus<File> f = dir.openat(lsn_to_filename(log_lsn), O_RDONLY)
                           .transform_error(to_db_error);
    if (!f) {
      warn("couldn't open logfile `{}`: {}", lsn_to_filename(log_lsn),
           f.error());
      ret = f.error();
      break;
    }

    uint64_t tmp = cur_lsn;
    ret = load_log_file(*f, cur_lsn, target_lsn, func);
    if (!ret) {
      warn("couldn't load logfile {} : {}", lsn_to_filename(log_lsn),
           ret.error());
      break;
    }

    if (tmp == cur_lsn) // empty log file
      break;
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
  if (loaded_lsn)
    *loaded_lsn = cur_lsn;
  return ret;
}

DBStatus<void>
DiskReader::apply_changes(uint64_t /*lsn*/, std::vector<UniqueLoglet> &&log)
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
