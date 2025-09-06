// hogdb_dao.h - provides a data access object (DAO) for hogdb's persistent
// state
//
// It abstracts away the low-level details of storing objects in the ramdb.
//
// TODO(abelay): We need to add support for account deletion, which should
// probably delete all keys associated with an account.

#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <span>

// xdr types
#include "db_coord.hh"
#include "nomination.hh"
#include "types.hh"

#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

// RAMDB is the low-level interface used by hogdb to access the ramdb.
class RAMDB {
public:
  RAMDB() = default;
  explicit RAMDB(std::unique_ptr<Database> db) : db_(std::move(db)) {}
  ~RAMDB() = default;

  RAMDB(RAMDB &&db) noexcept : db_(std::move(db.db_)) {}
  RAMDB &operator=(RAMDB &&db) noexcept
  {
    db_ = std::move(db.db_);
    return *this;
  }
  RAMDB(const RAMDB &db) = delete;
  RAMDB &operator=(const RAMDB &db) = delete;

  HogDBStatus<DBItem> get(std::span<const std::byte> key)
  {
    return db_->get(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<void> get_if_type(std::span<const std::byte> key, uint8_t type,
                                std::span<std::byte> val_out)
  {
    return db_->get_if_type(key, type, val_out).transform_error(to_hogdb_error);
  }

  HogDBStatus<OwningSpan<const std::byte>>
  get_if_type(std::span<const std::byte> key, uint8_t type)
  {
    return db_->get_if_type(key, type).transform_error(to_hogdb_error);
  }

  HogDBStatus<size_t> get_size(std::span<const std::byte> key)
  {
    return db_->get_size(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<uint8_t> get_type(std::span<const std::byte> key)
  {
    return db_->get_type(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<void> put(std::span<const std::byte> key, uint8_t type,
                        std::span<const std::byte> val)
  {
    return db_->put(key, type, val).transform_error(to_hogdb_error);
  }

  HogDBStatus<void> remove(std::span<const std::byte> key)
  {
    return db_->remove(key).transform_error(to_hogdb_error);
  }

  HogDBStatus<uint64_t> commit()
  {
    return db_->sync().transform_error(to_hogdb_error);
  }

  void get_each_parallel(GetEachCallback fn,
                         size_t width = kDefaultParallelRangeWidth,
                         WorkerPool &pool = WorkerPool::get_default_pool())
  {
    db_->get_each_parallel(std::move(fn), width, pool);
  }

private:
  std::unique_ptr<Database> db_;
};

// Item type identifiers; stored in the type field in ramdb.
//
// These are only used in cases where multiple types can be stored at an
// address. Currently this is for ItemKeys. Address balances are also marked as
// kNNInt128 since hogdb treats them like a special ItemKey for native tokens.
enum {
  kNNInt128 = 1,
  kBytes,
};

// hogDBDAO provides a data access object (DAO) for hogdb's persistent state.
class HogDBDAO {
public:
  HogDBDAO() = default;
  explicit HogDBDAO(RAMDB &&db) : db_(std::move(db)) {}

  // returns the low-level ramdb (direct access to the DB).
  RAMDB &get_ramdb() { return db_; }

  // commit persists all pending changes in durable storage.
  HogDBStatus<uint64_t> commit() { return db_.commit(); }

  //
  // item key operations (for any item key type)
  //

  HogDBStatus<size_t> get_item_size(const ItemKey &key)
  {
    return db_.get_size(key.as_bytes());
  }

  HogDBStatus<int8_t> get_item_type(const ItemKey &key)
  {
    return db_.get_type(key.as_bytes());
  }

  HogDBStatus<void> remove_item(const ItemKey &key)
  {
    assert(!key.is_address_balance());
    return db_.remove(key.as_bytes());
  }

  // readers
  HogDBStatus<int128_t> read_address_balance(const Address &addr);
  HogDBStatus<HogAge> read_address_average_hog_age(const Address &addr);
  HogDBStatus<SHA256Hash> read_address_wasm_hash(const Address &addr);
  HogDBStatus<OwningSpan<const std::byte>>
  read_wasm_code(const SHA256Hash &hash);
  HogDBStatus<int128_t> read_nnint128(const ItemKey &key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(const ItemKey &key);

  // writers
  HogDBStatus<void> write_address_balance(const Address &addr, int128_t val);
  HogDBStatus<void> write_address_average_hog_age(const HogAge &val);
  HogDBStatus<void> write_address_wasm_hash(const Address &addr,
                                            const SHA256Hash &hash);
  HogDBStatus<void> write_wasm_code(const SHA256Hash &hash,
                                    std::span<const std::byte> buf);
  HogDBStatus<void> write_nnint128(const ItemKey &key, int128_t val);
  HogDBStatus<void> write_bytes(const ItemKey &key,
                                std::span<const std::byte> buf);

private:
  RAMDB db_;
};

} // namespace hogd
