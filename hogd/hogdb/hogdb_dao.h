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
#include <optional>
#include <span>
#include <utility>
#include <vector>

// xdr types
#include "account.hh"
#include "db_coord.hh"
#include "db_sched.hh"
#include "hogd/base/os.h"
#include "hogd/base/worker_pool.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/invariant.h"
#include "hogd/hogdb/invariant_hog_conservation.h"
#include "hogd/storage_cost/storage_cost_model.h"
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

// Prefix is a prefix appended to each key to form an overall key that
// indicates the category of data being stored. The rest of the key is normally
// a contract address, except for kWasmCode, which uses the hash of the wasm
// blob as the rest of its key.
//
// Warning: These key prefixes are persisted on disk so they cannot be changed
// without migrating existing data.
//
// Warning: These prefixes are NOT included in the merkle trie root hash.
//
// Not every prefix type is included in the merkle trie.
enum class Prefix : char {
  kHogAge = 'A',
  kBalance = 'B',
  kAccountAuthData = 'D',
  kAddressMetadata = 'M',
  kContractDataKey = 'K',
  kWasmCode = 'C',
  kWasmCodeRefcount = 'R',
  // global info, like the db tip, have prefixes that begin with (and may
  // extend) this character
  kGlobalMetadata = '<',
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

  void enable_debug_invariants(StorageCostModel const &cost_model)
  {
    assert(invariants_.empty());
    invariants_.push_back(
        std::make_unique<HogConservationInvariant>(cost_model));
  }

  bool check_invariants()
  {
    bool ret = true;
    for (auto &invariant : invariants_) {
      if (!invariant->check()) {
        ret = false;
        break;
      }
    }
    invariants_.clear();
    return ret;
  }

  // for_each_state_item calls fn in parallel for every item that needs to be
  // included in the merkle state root. It is typically used at initialization
  // time when the database is first loaded.
  void for_each_state_item(GetEachCallback fn);

  // loads the trie from the database.
  // Recomputes the top-level hashes.
  void load_trie(MerkleTriePrefix &trie);

  //
  // Global Operations
  //

  HogDBStatus<DbTip> read_dbtip();
  HogDBStatus<void> write_dbtip(const DbTip &tip);

  HogDBStatus<StorageCostModel> read_cost_model();

  //
  // Address Operations
  //

  HogDBStatus<int128_t> read_address_balance(const Address &addr);
  HogDBStatus<HogAge> read_address_average_hog_age(const Address &addr);
  HogDBStatus<AccountAuthData>
  read_address_account_auth_data(const Address &addr);
  HogDBStatus<DeployedContractHash> read_address_metadata(const Address &addr);

  HogDBStatus<void> write_address_balance(const Address &addr, int128_t val);
  HogDBStatus<void> write_address_average_hog_age(const HogAge &age);
  HogDBStatus<void>
  write_address_account_auth_data(const Address &addr,
                                  const AccountAuthData &auth);
  HogDBStatus<void> write_address_metadata(Address const &addr,
                                           const DeployedContractHash &hash);

  //
  // WASM Operations
  //

  HogDBStatus<OwningSpan<const std::byte>>
  read_wasm_code(const SHA256Hash &hash);
  HogDBStatus<void> write_wasm_code(const SHA256Hash &hash,
                                    std::span<const std::byte> buf);

  //
  // ItemKey Operations
  //

  HogDBStatus<int128_t> read_nnint128(const ItemKey &key);
  HogDBStatus<OwningSpan<const std::byte>> read_bytes(const ItemKey &key);
  // read any of the item types
  HogDBStatus<DBItem> read_contract_data_item(const ItemKey &key);

  HogDBStatus<void> write_nnint128(const ItemKey &key, int128_t val);
  HogDBStatus<void> write_bytes(const ItemKey &key,
                                std::span<const std::byte> buf);

  HogDBStatus<size_t>
  get_charged_storage_cost(const ItemKey &key,
                           StorageCostModel const *cost_model);
  HogDBStatus<int8_t> get_item_type(const ItemKey &key);

  HogDBStatus<void> remove_item(const ItemKey &key);

private:
  std::optional<HogDBStatus<DBItem>>
  get_before_item(std::span<const std::byte> key)
  {
    if (!invariants_.empty()) {
      return db_.get(key);
    }
    return std::nullopt;
  }

  void update_invariants_post(std::span<const std::byte> key,
                              std::optional<HogDBStatus<DBItem>> const &before)
  {
    if (!invariants_.empty()) {
      assert(before.has_value());
      auto after = db_.get(key);
      for (auto &invariant : invariants_) {
        invariant->log_item_change(key, *before, after);
      }
    }
  }

  std::vector<std::unique_ptr<HogdbInvariant>> invariants_;
  RAMDB db_;
};

} // namespace hogd
