#include "hogd/hogdb/hogdb_dao.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>

// xdr types
#include "db_coord.hh"
#include "types.hh"

#include "hogd/base/bits.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr std::string kGlobalTipKey = ">DBTIP";

// Prefix is a prefix appended to each key to form an overall key that
// indicates the category of data being stored. The rest of the key is normally
// a contract address, except for kWasmCode, which uses the hash of the wasm
// blob as the rest of its key.
//
// Warning: These key prefixes are persisted on disk so they cannot be changed
// without migrating existing data.
//
// Warning: So far, they are also included in the merkle trie root hash, so
// they're very hard to change.
enum class Prefix : char {
  kHogAge = 'A',
  kBalance = 'B',
  kWasmHash = 'H',
  kWasmCode = 'C',
};

// Warning: you may have to update this if you make changes to Prefix.
constexpr bool
is_state_item(std::span<const std::byte> key)
{
  // skip global state.
  if (static_cast<char>(key[0]) == '>')
    return false;

  // we infer an ItemKey based on this size.
  if (key.size() == kAddressSize + kItemKeySize)
    return true;

  // we infer an Address with prefix based on this size.
  if (key.size() == kAddressSize + 1)
    return static_cast<char>(key[0]) != static_cast<char>(Prefix::kHogAge);

  return false;
}

constexpr auto
make_address_key(const Address &addr, Prefix p)
{
  std::array<uint8_t, kAddressSize + 1> ret;
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1);
  ret[0] = static_cast<char>(p);
  return ret;
}

constexpr auto
make_wasm_code_key(const SHA256Hash &hash)
{
  std::array<uint8_t, hash.size() + 1> ret;
  std::ranges::copy(hash, std::begin(std::span(ret)) + 1);
  ret[0] = static_cast<char>(Prefix::kWasmCode);
  return ret;
}

} // namespace

void
HogDBDAO::for_each_state_item(GetEachCallback fn)
{
  db_.get_each_parallel([&fn](std::span<const std::byte> key, uint8_t type,
                              std::span<const std::byte> value) {
    if (is_state_item(key))
      fn(key, type, value);
  });
}

HogDBStatus<DbTip>
HogDBDAO::read_dbtip()
{
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalTipKey)), 0);
  if (!raw)
    return raw.error();

  DbTip tip;
  try {
    x23::deserialize(&tip, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return tip;
}

HogDBStatus<void>
HogDBDAO::write_dbtip(const DbTip &tip)
{
  auto buf = x23::serialize(tip);
  return db_.put(std::as_bytes(std::span(kGlobalTipKey)), 0,
                 std::as_bytes(std::span(buf)));
}

HogDBStatus<int128_t>
HogDBDAO::read_address_balance(const Address &addr)
{
  int128_t val;
  auto key = make_address_key(addr, Prefix::kBalance);
  HogDBStatus<void> ret = db_.get_if_type(std::as_bytes(std::span(key)),
                                          kNNInt128, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

HogDBStatus<HogAge>
HogDBDAO::read_address_average_hog_age(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kHogAge);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  HogAge age;
  try {
    x23::deserialize(&age, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return age;
}

HogDBStatus<SHA256Hash>
HogDBDAO::read_address_wasm_hash(const Address &addr)
{
  SHA256Hash val;
  auto key = make_address_key(addr, Prefix::kWasmHash);
  HogDBStatus<void> ret = db_.get_if_type(std::as_bytes(std::span(key)), 0,
                                          writable_byte_view(val));
  if (!ret)
    return ret.error();
  return val;
}

HogDBStatus<OwningSpan<const std::byte>>
HogDBDAO::read_wasm_code(const SHA256Hash &hash)
{
  auto key = make_wasm_code_key(hash);
  return db_.get_if_type(std::as_bytes(std::span(key)), 0);
}

HogDBStatus<int128_t>
HogDBDAO::read_nnint128(const ItemKey &key)
{
  if (key.is_address_balance())
    return read_address_balance(key.get_address());

  int128_t val;
  HogDBStatus<void> ret =
      db_.get_if_type(key.as_bytes(), kNNInt128, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

HogDBStatus<OwningSpan<const std::byte>>
HogDBDAO::read_bytes(const ItemKey &key)
{
  return db_.get_if_type(key.as_bytes(), kBytes);
}

HogDBStatus<void>
HogDBDAO::write_address_balance(const Address &addr, int128_t val)
{
  auto key = make_address_key(addr, Prefix::kBalance);
  return db_.put(std::as_bytes(std::span(key)), kNNInt128,
                 byte_view(cpu_to_le(val)));
}

HogDBStatus<void>
HogDBDAO::write_address_average_hog_age(const HogAge &age)
{
  auto key = make_address_key(age.address, Prefix::kHogAge);
  auto buf = x23::serialize(age);
  return db_.put(std::as_bytes(std::span(key)), 0,
                 std::as_bytes(std::span(buf)));
}

HogDBStatus<void>
HogDBDAO::write_address_wasm_hash(const Address &addr, const SHA256Hash &hash)
{
  auto key = make_address_key(addr, Prefix::kWasmHash);
  return db_.put(std::as_bytes(std::span(key)), 0, byte_view(hash));
}

HogDBStatus<void>
HogDBDAO::write_wasm_code(const SHA256Hash &hash,
                          std::span<const std::byte> buf)
{
  auto key = make_wasm_code_key(hash);
  return db_.put(std::as_bytes(std::span(key)), 0, buf);
}

HogDBStatus<void>
HogDBDAO::write_nnint128(const ItemKey &key, int128_t val)
{
  if (key.is_address_balance())
    return write_address_balance(key.get_address(), val);

  return db_.put(key.as_bytes(), kNNInt128, byte_view(cpu_to_le(val)));
}

HogDBStatus<void>
HogDBDAO::write_bytes(const ItemKey &key, std::span<const std::byte> buf)
{
  return db_.put(key.as_bytes(), kBytes, buf);
}

} // namespace hogd
