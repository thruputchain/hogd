// db_hash.h - helpers for converting between 32-byte hashes and DbHash.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hogd/crypto/lthash16.h"
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

} // namespace hogd
