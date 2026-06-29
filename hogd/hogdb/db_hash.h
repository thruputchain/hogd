#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>

#include "hogd/crypto/lthash16.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "rpc_types.hh"
#include "x23/vec.h"

namespace hogd {

inline DbHash
db_hash_from_lthash16(const LtHash16 &hash)
{
  DbHash out{};
  static_assert(sizeof(out) == LtHash16::kStateBytes);
  std::memcpy(out.data(), hash.state().data(), LtHash16::kStateBytes);
  return out;
}

inline LtHash16
lthash16_from_db_hash(const DbHash &hash)
{
  LtHash16::State state{};
  static_assert(sizeof(hash) == LtHash16::kStateBytes);
  std::memcpy(state.data(), hash.data(), LtHash16::kStateBytes);
  return LtHash16(state);
}

inline DbHash
db_hash_from_buf(x23::BufView buf)
{
  LtHash16 hash;
  hash.add(buf);
  return db_hash_from_lthash16(hash);
}

// The element encoding key || type || value lives in exactly one place:
// LtHash16's piecewise (streaming) overload. These helpers only add the
// is_state_item guard and delegate, so the full-rehash path and the incremental
// ChangeSet path share a single code path and agree by construction -- with no
// per-item buffer allocation.
inline void
lthash_add_item(LtHash16 &hash, std::span<const std::byte> key, uint8_t type,
                std::span<const std::byte> value)
{
  if (!is_state_item(key)) {
    return;
  }
  hash.add(key, type, value);
}

inline void
lthash_remove_item(LtHash16 &hash, std::span<const std::byte> key, uint8_t type,
                   std::span<const std::byte> value)
{
  if (!is_state_item(key)) {
    return;
  }
  hash.remove(key, type, value);
}

inline void
replace_known_item(LtHash16 &hash, HogDBDAO &db, std::span<const std::byte> key,
                   uint8_t type, std::span<const std::byte> new_value)
{
  auto before = db.get_ramdb().get(key);
  if (before.has_value()) {
    lthash_remove_item(hash, key, before->type, before->value);
  }
  lthash_add_item(hash, key, type, new_value);
}

} // namespace hogd
