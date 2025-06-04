// db.h - wraps the ramdb interface with operations relevant to hogdb.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include "hogd/base/bits.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"

namespace hogd {

// HogDatabase is the main interface used by hogdb to access the ramdb.
class HogDatabase {
public:
  HogDatabase(std::unique_ptr<Database> db) : db_(std::move(db)) {}
  ~HogDatabase() = default;

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

private:
  std::unique_ptr<Database> db_;
};

// Type identifiers; stored directly in the ramdb.
enum {
  kNNInt128 = 0,
  kBytes,
  kWASMCode,
  kSHA256Hash,
};

namespace detail {

constexpr auto
make_address_key(const Address &addr, char c)
{
  std::array<uint8_t, kAddressSize + 2> ret;
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 2);
  ret[0] = 'A';
  ret[1] = c;
  return ret;
}

constexpr auto
make_code_key(const SHA256Hash &hash)
{
  std::array<uint8_t, hash.size() + 1> ret;
  std::ranges::copy(hash, std::begin(std::span(ret)) + 1);
  ret[0] = 'C';
  return ret;
}

} // namespace detail

//
// Address Operations
//

inline HogDBStatus<int128_t>
read_address_balance(HogDatabase &db, const Address &addr)
{
  int128_t val;
  auto key = detail::make_address_key(addr, 'B');
  HogDBStatus<void> ret = db.get_if_type(std::as_bytes(std::span(key)),
                                         kNNInt128, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

inline HogDBStatus<void>
write_address_balance(HogDatabase &db, const Address &addr, int128_t val)
{
  auto key = detail::make_address_key(addr, 'B');
  return db.put(std::as_bytes(std::span(key)), kNNInt128,
                byte_view(cpu_to_le(val)));
}

inline HogDBStatus<SHA256Hash>
read_address_wasm_hash(HogDatabase &db, const Address &addr)
{
  SHA256Hash val;
  auto key = detail::make_address_key(addr, 'C');
  HogDBStatus<void> ret = db.get_if_type(std::as_bytes(std::span(key)),
                                         kSHA256Hash, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return val;
}

inline HogDBStatus<void>
write_address_wasm_hash(HogDatabase &db, const Address &addr,
                        const SHA256Hash &hash)
{
  auto key = detail::make_address_key(addr, 'C');
  return db.put(std::as_bytes(std::span(key)), kSHA256Hash, byte_view(hash));
}

//
// WASM Code Operations
//

inline HogDBStatus<OwningSpan<const std::byte>>
read_wasm_code(HogDatabase &db, const SHA256Hash &hash)
{
  auto key = detail::make_code_key(hash);
  return db.get_if_type(std::as_bytes(std::span(key)), kWASMCode);
}

inline HogDBStatus<void>
write_wasm_code(HogDatabase &db, const SHA256Hash &hash,
                std::span<const std::byte> buf)
{
  auto key = detail::make_code_key(hash);
  return db.put(std::as_bytes(std::span(key)), kWASMCode, buf);
}

//
// Generic Storage Item Operations
//

inline HogDBStatus<size_t>
get_item_size(HogDatabase &db, const ItemKey &key)
{
  return db.get_size(key.as_bytes());
}

inline HogDBStatus<void>
remove_item(HogDatabase &db, const ItemKey &key)
{
  assert(!key.is_address_balance());
  return db.remove(key.as_bytes());
}

//
// NNInt128 Storage Item Operations
//

inline HogDBStatus<int128_t>
read_nnint128(HogDatabase &db, const ItemKey &key)
{
  if (key.is_address_balance())
    return read_address_balance(db, key.get_address());

  int128_t val;
  HogDBStatus<void> ret =
      db.get_if_type(key.as_bytes(), kNNInt128, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

inline HogDBStatus<void>
write_nnint128(HogDatabase &db, const ItemKey &key, int128_t val)
{
  if (key.is_address_balance())
    return write_address_balance(db, key.get_address(), val);

  return db.put(key.as_bytes(), kNNInt128, byte_view(cpu_to_le(val)));
}

//
// Byte Storage Item Operations
//

inline HogDBStatus<OwningSpan<const std::byte>>
read_bytes(HogDatabase &db, const ItemKey &key)
{
  return db.get_if_type(key.as_bytes(), kBytes);
}

inline HogDBStatus<void>
write_bytes(HogDatabase &db, const ItemKey &key, std::span<const std::byte> buf)
{
  return db.put(key.as_bytes(), kBytes, buf);
}

} // namespace hogd
