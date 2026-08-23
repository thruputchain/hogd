// xdr_util.h - utilities for converting to and from xdr types

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>
#include <vector>
#include <version>

#include "hogd/base/bits.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "parallel.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

constexpr int128_xdr
to_xdr(int128_t value)
{
  return int128_xdr{.highbits = static_cast<int64_t>(value >> 64),
                    .lowbits = static_cast<uint64_t>(value & UINT64_MAX)};
}

constexpr uint128_xdr
to_xdr(uint128_t value)
{
  return uint128_xdr{.highbits = static_cast<uint64_t>(value >> 64),
                     .lowbits = static_cast<uint64_t>(value & UINT64_MAX)};
}

constexpr int128_t
from_xdr(int128_xdr value)
{
  return (int128_t{value.highbits} << 64) + value.lowbits;
}

constexpr uint128_t
from_xdr(uint128_xdr value)
{
  return (uint128_t{value.highbits} << 64) + value.lowbits;
}

// #1846, #1874: verify shift, cast, and round-trip semantics that
// XDR conversion and native-token precompile arithmetic depend on.
static_assert(int128_t{-1} >> 64 == -1, "arithmetic right shift required");
static_assert(static_cast<int64_t>(int128_t{-1} >> 64) == int64_t{-1},
              "int128 high-limb cast must preserve sign");
static_assert(static_cast<uint64_t>(int128_t{-1} & UINT64_MAX) == UINT64_MAX,
              "int128 low-limb mask must yield all-ones");
static_assert(to_xdr(int128_t{-1}).highbits == int64_t{-1},
              "to_xdr high limb mismatch");
static_assert(to_xdr(int128_t{-1}).lowbits == UINT64_MAX,
              "to_xdr low limb mismatch");
static_assert(from_xdr(to_xdr(int128_t{0})) == 0, "XDR round-trip failed");
static_assert(from_xdr(to_xdr(int128_t{-1})) == -1, "XDR round-trip failed");
static_assert(from_xdr(to_xdr(int128_t{1})) == 1, "XDR round-trip failed");
static_assert(from_xdr(to_xdr(int128_t{1} << 127)) == (int128_t{1} << 127),
              "XDR round-trip failed for INT128_MIN");
static_assert(from_xdr(to_xdr((int128_t{INT64_MAX} << 64) | UINT64_MAX)) ==
                  (int128_t{INT64_MAX} << 64 | UINT64_MAX),
              "XDR round-trip failed for INT128_MAX");
// limb reconstruction used by precompile_native_token.cc
static_assert((uint128_t{UINT64_MAX} << 64 | UINT64_MAX) ==
                  static_cast<uint128_t>(int128_t{-1}),
              "uint128 all-ones must match int128 -1");
static_assert((int128_t{int64_t{-1}} << 64 | int128_t{UINT64_MAX}) == -1,
              "signed limb reconstruction must produce -1");

// #3070: NNInt128 and balance rows are persisted and folded into the
// consensus DbHash as `int128_to_xdr_bytes(...)`, whose width is
// `sizeof(int128_xdr)`. `int128_xdr` is two 64-bit limbs with no over-
// alignment, so on every conforming target it has no tail padding and this
// width is exactly the 16-byte fixed XDR representation of the two limbs.
// Pin that invariant at compile time so the persisted/hashed row width can
// never drift from the wire encoding regardless of host object layout.
static_assert(sizeof(int128_xdr) == 16,
              "int128_xdr must be exactly two 64-bit limbs with no tail "
              "padding; persisted/hashed row width feeds the consensus DbHash");
static_assert(sizeof(int128_xdr) == sizeof(uint128_xdr),
              "signed and unsigned 128-bit row widths must match");

// #3131: the fixed-width helpers below store their `std::byte` row storage
// and read it back through `uint8_t` (via `x23::putbe`/`getbe`, which
// dereference `uint8_t` elements). Object-representation access through
// `uint8_t` is only alias-permitted when `uint8_t` is a character type. Every
// target we build on typedefs `std::uint8_t` to `unsigned char`, but pin that
// precondition at compile time so the consensus-state persistence/DbHash path
// can never silently rely on an implementation where `std::uint8_t` is an
// extended (non-character) integer type.
static_assert(std::is_same_v<std::uint8_t, unsigned char>,
              "NNInt128 XDR byte helpers access std::byte storage through "
              "uint8_t; uint8_t must be unsigned char for that access to be "
              "alias-safe in the consensus DbHash path");

inline std::array<std::byte, sizeof(int128_xdr)>
int128_to_xdr_bytes(int128_t value)
{
  int128_xdr xdr = to_xdr(value);
  std::array<std::byte, sizeof(int128_xdr)> out{};
  auto *raw = reinterpret_cast<uint8_t *>(out.data());

  x23::putbe(raw, std::bit_cast<uint64_t>(xdr.highbits));
  x23::putbe(raw + sizeof(uint64_t), xdr.lowbits);
  return out;
}

inline int128_t
int128_from_xdr_bytes(std::span<const std::byte, sizeof(int128_xdr)> bytes)
{
  auto const *raw = reinterpret_cast<uint8_t const *>(bytes.data());
  int128_xdr xdr = {
      .highbits = std::bit_cast<int64_t>(x23::getbe<uint64_t>(raw)),
      .lowbits = x23::getbe<uint64_t>(raw + sizeof(uint64_t)),
  };
  return from_xdr(xdr);
}

template<typename T>
constexpr T
xdr_le_to_cpu(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = le_to_cpu(value.highbits);
  value.lowbits = le_to_cpu(value.lowbits);
  return value;
}

template<typename T>
constexpr T
xdr_cpu_to_le(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = cpu_to_le(value.highbits);
  value.lowbits = cpu_to_le(value.lowbits);
  return value;
}

template<typename T>
constexpr T
xdr_be_to_cpu(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = be_to_cpu(value.highbits);
  value.lowbits = be_to_cpu(value.lowbits);
  return value;
}

template<typename T>
constexpr T
xdr_cpu_to_be(T value)
{
  static_assert(
      std::is_same_v<T, int128_xdr> || std::is_same_v<T, const int128_xdr> ||
          std::is_same_v<T, uint128_xdr> ||
          std::is_same_v<T, const uint128_xdr>,
      "[T] must be [int128_xdr] or [uint128_xdr] (const or non-const).");

  value.highbits = cpu_to_be(value.highbits);
  value.lowbits = cpu_to_be(value.lowbits);
  return value;
}

// The number of coroutines to use (one coroutine per thread) when converting
// to/from a parallel batch.
constexpr size_t kNumThreadsForParallelBatch = 20;

// Decodes a ParallelBatch, returning false for any malformed input: metadata
// mismatches (segment_tokens/segment_bytes size, total segment bytes vs
// xdr.size()) as well as per-object XDR deserialization failures.
template<typename T>
inline x23::Async<bool>
parallel_batch_to_t(const ParallelBatch &parallel, x23::XdrVec<T> &objs)
{
  if (parallel.segment_tokens.size() != parallel.segment_bytes.size()) {
    info("failed to deserialize: segment_tokens size {} != segment_bytes size "
         "{}",
         parallel.segment_tokens.size(), parallel.segment_bytes.size());
    co_return false;
  }

  std::vector<size_t> byte_offsets;
  byte_offsets.reserve(parallel.segment_bytes.size());
  size_t total_segment_bytes = 0;
  for (unsigned int segment_bytes : parallel.segment_bytes) {
    byte_offsets.push_back(total_segment_bytes);
    total_segment_bytes += segment_bytes;
  }
  if (total_segment_bytes != parallel.xdr.size()) {
    info("failed to deserialize: total segment bytes {} != xdr size {}",
         total_segment_bytes, parallel.xdr.size());
    co_return false;
  }

  // Each decoded object consumes at least [kMinObjectSize] bytes of [xdr], so
  // the payload can encode at most [total_segment_bytes / kMinObjectSize]
  // objects, and an XDR vector can hold at most 0xffffffff objects regardless.
  // Enforcing this bound before [objs.resize] below stops a small batch from
  // overclaiming segment tokens to force an enormous allocation (#1645). We
  // divide rather than multiply so the bound cannot overflow [size_t]. The sum
  // itself cannot overflow either: [segment_tokens] holds at most 0xffffffff
  // entries each at most 0xffffffff, so the total is below SIZE_MAX.
  constexpr size_t kMinObjectSize = x23::detail::xdr_size_bounds<T>().min_;
  static_assert(kMinObjectSize > 0,
                "ParallelBatch element type must have a nonzero minimum XDR "
                "size, else token counts cannot bound the allocation");
  const size_t max_total_segment_tokens =
      std::min<size_t>(0xffffffffUL, total_segment_bytes / kMinObjectSize);

  std::vector<size_t> obj_offsets;
  obj_offsets.reserve(parallel.segment_tokens.size());
  size_t total_segment_tokens = 0;
  for (unsigned int segment_tokens : parallel.segment_tokens) {
    obj_offsets.push_back(total_segment_tokens);
    total_segment_tokens += segment_tokens;
  }
  if (total_segment_tokens > max_total_segment_tokens) {
    info("failed to deserialize: claimed {} segment tokens exceeds the maximum "
         "{} that {} payload bytes can encode",
         total_segment_tokens, max_total_segment_tokens, total_segment_bytes);
    co_return false;
  }
  objs.resize(total_segment_tokens, {});

  const std::vector<ChunkRange> chunks =
      make_chunks(parallel.segment_tokens.size(), kNumThreadsForParallelBatch);
  x23::Vec<x23::Async<bool>> handles;
  handles.reserve(chunks.size());
  for (const ChunkRange &chunk : chunks) {
    handles.push_back([](const ChunkRange &chunk, const ParallelBatch &parallel,
                         x23::XdrVec<T> &objs,
                         const std::vector<size_t> &byte_offsets,
                         const std::vector<size_t> &obj_offsets) noexcept
                          -> x23::Async<bool> {
      co_await default_coropoll().pool_.bg();

      for (size_t i = chunk.start; i < chunk.end; i++) {
        const unsigned int num_segment_tokens = parallel.segment_tokens[i];
        const unsigned int num_segment_bytes = parallel.segment_bytes[i];

        size_t inner_obj_offset = 0;
        size_t inner_xdr_offset = 0;
        for (size_t j = 0; j < num_segment_tokens; j++) {
          if (inner_xdr_offset >= num_segment_bytes) {
            info("failed to deserialize: segment {} claims {} tokens but "
                 "byte offset {} reached segment size {}",
                 i, num_segment_tokens, inner_xdr_offset, num_segment_bytes);
            co_return false;
          }

          try {
            const size_t obj_idx = obj_offsets[i] + inner_obj_offset;
            size_t num_consumed = x23::deserialize_from(
                &objs[obj_idx],
                x23::BufView(&parallel.xdr[byte_offsets[i] + inner_xdr_offset],
                             num_segment_bytes - inner_xdr_offset));
            inner_obj_offset++;
            inner_xdr_offset += num_consumed;
            // if inner_xdr_offset > num_segment_bytes, then deserialize_from
            // would have read beyond the end of the supplied buffer.
            CHECK_LE(inner_xdr_offset, num_segment_bytes);
          } catch (const x23::XdrException &e) {
            info("failed to deserialize: {}", e.what());
            co_return false;
          }
        }

        if (inner_xdr_offset != num_segment_bytes) {
          info("failed to deserialize: segment {} left {} trailing bytes", i,
               num_segment_bytes - inner_xdr_offset);
          co_return false;
        }
      }
      co_return true;
    }(chunk, parallel, objs, byte_offsets, obj_offsets));
  }
  bool ret = true;
  for (x23::Async<bool> &handle : handles) {
    // Careful, we deliberately have [co_await handle] as the first operand of
    // the AND statement to avoid short-circuiting. If we did it the other way
    // around ([ret && (co_await handle)]), then if any of the handles return
    // false and we set [ret] to false, we would fail to [co_await] the
    // remaining handles in [handles] due to short-circuiting.
    ret = (co_await handle) && ret;
  }
  co_return ret;
}

template<typename T>
inline x23::Async<void>
append_to_parallel_batch(x23::span<T> objs, ParallelBatch &parallel)
{
  // [t] and [{t}] (i.e., a vector with [t]) serialize differently. Thus, we
  // serialize each object individually rather than serialize [objs] once.
  const std::vector<ChunkRange> chunks =
      make_chunks(objs.size(), kNumThreadsForParallelBatch);
  const size_t segment_offset = parallel.segment_tokens.size();
  x23::Vec<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  x23::Vec<x23::XdrVec<std::uint8_t>> xdrs;
  xdrs.resize(chunks.size(), {});
  parallel.segment_bytes.resize(segment_offset + chunks.size(), 0);
  parallel.segment_tokens.resize(segment_offset + chunks.size(), 0);
  for (size_t i = 0; i < chunks.size(); i++) {
    const ChunkRange &chunk = chunks[i];
    handles.push_back([](const ChunkRange &chunk, size_t i,
                         size_t segment_offset, ParallelBatch &parallel,
                         x23::Vec<x23::XdrVec<std::uint8_t>> &xdrs,
                         x23::span<T> &objs) noexcept -> x23::Async<void> {
      co_await default_coropoll().pool_.bg();

      const size_t segment_idx = segment_offset + i;
      parallel.segment_tokens[segment_idx] = chunk.end - chunk.start;
      // This is just an educated guess, but hopefully it lowers/eliminates the
      // resize overhead of the vector.
      xdrs[i].reserve(parallel.segment_tokens[segment_idx] * sizeof(T) * 3);
      for (size_t j = chunk.start; j < chunk.end; j++) {
        xdrs[i] += x23::serialize(objs[j]);
      }
      parallel.segment_bytes[segment_idx] = xdrs[i].size();
      co_return;
    }(chunk, i, segment_offset, parallel, xdrs, objs));
  }
  // This is just an educated guess, but hopefully it lowers/eliminates the
  // resize overhead of the vector.
  parallel.xdr.reserve(parallel.xdr.size() + (objs.size() * sizeof(T) * 3));
  for (size_t i = 0; i < chunks.size(); i++) {
    co_await handles[i];
    parallel.xdr += xdrs[i];
  }
  co_return;
}

template<typename T>
inline x23::Async<void>
t_to_parallel_batch(x23::span<T> objs, ParallelBatch &parallel)
{
  // [ParallelBatch] is a struct, and the caller likely allocated it without
  // zeroing it out. Thus, we zero it out here.
  parallel = {};
  co_await append_to_parallel_batch(objs, parallel);
  co_return;
}

template<typename T>
inline x23::Async<ParallelBatch>
t_to_parallel_batch(x23::span<T> objs)
{
  ParallelBatch parallel;
  co_await t_to_parallel_batch(objs, parallel);
  co_return parallel;
}

template<typename T>
inline x23::Async<void>
append_to_parallel_batch(const x23::XdrVec<T> &objs, ParallelBatch &parallel)
{
  co_await append_to_parallel_batch(x23::span<const T>(objs), parallel);
  co_return;
}

template<typename T>
inline x23::Async<void>
t_to_parallel_batch(const x23::XdrVec<T> &objs, ParallelBatch &parallel)
{
  co_await t_to_parallel_batch(x23::span<const T>(objs), parallel);
  co_return;
}

template<typename T>
inline x23::Async<ParallelBatch>
t_to_parallel_batch(const x23::XdrVec<T> &objs)
{
  ParallelBatch parallel;
  co_await t_to_parallel_batch(x23::span<const T>(objs), parallel);
  co_return parallel;
}

template<std::size_t N>
auto
show_byte_array(const std::array<uint8_t, N> &array)
{
#if (__cpp_lib_format_ranges >= 202207L)
  return array;
#else
  return encode_hex(std::as_bytes(std::span(array)));
#endif
}

} // namespace hogd

template<> struct std::hash<hogd::AddressAndKey> {
  std::size_t operator()(const hogd::AddressAndKey &addrkey) const noexcept
  {
    auto key_disc = addrkey.key.key_type();

    static_assert(sizeof(addrkey.address) == 32,
                  "ContractAddress size changed; update buf allocation");
    std::array<uint8_t, 32 + sizeof(key_disc) + 32> buf{};
    size_t pos = 0;

    std::memcpy(buf.data() + pos, addrkey.address.data(),
                addrkey.address.size());
    pos += addrkey.address.size();

    std::memcpy(buf.data() + pos, &key_disc, sizeof(key_disc));
    pos += sizeof(key_disc);

    if (key_disc == hogd::StorageKeyType::ContractData) {
      static_assert(
          sizeof(addrkey.key.contract_data_key()) == 32,
          "StorageKeyContractData size changed; update buf allocation");
      std::memcpy(buf.data() + pos, addrkey.key.contract_data_key().data(),
                  addrkey.key.contract_data_key().size());
      pos += addrkey.key.contract_data_key().size();
    }
    else {
      CHECK(key_disc == hogd::StorageKeyType::FreeNativeTokenBalance ||
            key_disc == hogd::StorageKeyType::AuthData ||
            key_disc == hogd::StorageKeyType::Metadata ||
            key_disc == hogd::StorageKeyType::ReplayCache);
    }

    return hogd::SipHash24::get_default()(
        std::as_bytes(std::span(buf.data(), pos)));
  }
};
