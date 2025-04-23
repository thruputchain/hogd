// db.h - wraps the ramdb interface with operations relevant to hogdb.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"

namespace hogd {

// ItemKey is a unique identifier for each item in the ramdb.
struct ItemKey {
  uint128_t addr;
  uint128_t key;

  bool operator==(const ItemKey &rhs) const
  {
    return addr == rhs.addr && key == rhs.key;
  }
  std::span<const std::byte> as_bytes() const { return byte_view(*this); }
};

} // namespace hogd

template<> struct std::hash<hogd::ItemKey> {
  std::size_t operator()(const hogd::ItemKey &key) const
  {
    return hogd::SipHash24::get_default()(hogd::byte_view(key));
  }
};

namespace hogd {

// Type identifiers; stored directly in the ramdb.
enum {
  kNNInt128 = 0,
  kBytes,
};

// HogDatabase is the main interface used by hogdb to access the ramdb.
class HogDatabase {
public:
  HogDatabase(std::unique_ptr<Database> db) : db_(std::move(db)) {}
  ~HogDatabase() = default;

  // item readers (from the snapshot state)
  HogDBStatus<int128_t> read_nnint128(ItemKey key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(ItemKey key);

  // item writers
  HogDBStatus<void> write_nnint128(ItemKey key, int128_t val);
  HogDBStatus<void> write_bytes(ItemKey key, std::span<const std::byte> buf);
  HogDBStatus<void> remove(ItemKey key);

  // commit makes the writes durable and returns the next LSN.
  HogDBStatus<uint64_t> commit();

private:
  std::unique_ptr<Database> db_;
};

inline HogDBStatus<int128_t>
HogDatabase::read_nnint128(ItemKey key)
{
  int128_t val;
  HogDBStatus<void> ret =
      db_->get_if_type(key.as_bytes(), kNNInt128, writable_byte_view(val))
          .transform_error(to_hogdb_error);
  if (!ret)
    return ret.error();
  return val;
}

inline HogDBStatus<OwningSpan<const std::byte>>
HogDatabase::read_bytes(ItemKey key)
{
  return db_->get_if_type(key.as_bytes(), kBytes)
      .transform_error(to_hogdb_error);
}

inline HogDBStatus<void>
HogDatabase::write_nnint128(ItemKey key, int128_t val)
{
  return db_->put(key.as_bytes(), kNNInt128, byte_view(val))
      .transform_error(to_hogdb_error);
}

inline HogDBStatus<void>
HogDatabase::write_bytes(ItemKey key, std::span<const std::byte> buf)
{
  return db_->put(key.as_bytes(), kBytes, buf).transform_error(to_hogdb_error);
}

inline HogDBStatus<void>
HogDatabase::remove(ItemKey key)
{
  return db_->remove(key.as_bytes()).transform_error(to_hogdb_error);
}

inline HogDBStatus<uint64_t>
HogDatabase::commit()
{
  return db_->sync().transform_error(to_hogdb_error);
}

} // namespace hogd
