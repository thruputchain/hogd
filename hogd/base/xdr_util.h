// xdr_util.h - utilities for converting to and from xdr types

#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>
#include <version>

#include "hogd/base/bits.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
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

template<typename T>
inline x23::Async<bool>
parallel_batch_to_t(const ParallelBatch &parallel, x23::XdrVec<T> &objs)
{
  CHECK_EQ(parallel.segment_tokens.size(), parallel.segment_bytes.size());

  std::vector<size_t> byte_offsets;
  byte_offsets.reserve(parallel.segment_bytes.size());
  size_t total_segment_bytes = 0;
  for (unsigned int segment_bytes : parallel.segment_bytes) {
    byte_offsets.push_back(total_segment_bytes);
    total_segment_bytes += segment_bytes;
  }
  CHECK_EQ(total_segment_bytes, parallel.xdr.size());

  std::vector<size_t> obj_offsets;
  obj_offsets.reserve(parallel.segment_tokens.size());
  size_t total_segment_tokens = 0;
  for (unsigned int segment_tokens : parallel.segment_tokens) {
    obj_offsets.push_back(total_segment_tokens);
    total_segment_tokens += segment_tokens;
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
          try {
            size_t num_consumed = x23::deserialize_from(
                &objs[obj_offsets[i] + inner_obj_offset++],
                x23::BufView(&parallel.xdr[byte_offsets[i] + inner_xdr_offset],
                             num_segment_bytes - inner_xdr_offset));
            inner_xdr_offset += num_consumed;
          } catch (const x23::XdrBadInput &e) {
            info("failed to deserialize: {}", e.what());
            co_return false;
          }
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
t_to_parallel_batch(x23::span<T> objs, ParallelBatch &parallel)
{
  // [t] and [{t}] (i.e., a vector with [t]) serialize differently. Thus, we
  // serialize each object individually rather than serialize [objs] once.
  // [ParallelBatch] is a struct, and the caller likely allocated it without
  // zeroing it out. Thus, we zero it out here.
  parallel = {};
  const std::vector<ChunkRange> chunks =
      make_chunks(objs.size(), kNumThreadsForParallelBatch);
  x23::Vec<x23::Async<void>> handles;
  handles.reserve(chunks.size());
  x23::Vec<x23::XdrVec<std::uint8_t>> xdrs;
  xdrs.resize(chunks.size(), {});
  parallel.segment_bytes.resize(chunks.size(), 0);
  parallel.segment_tokens.resize(chunks.size(), 0);
  for (size_t i = 0; i < chunks.size(); i++) {
    const ChunkRange &chunk = chunks[i];
    handles.push_back([](const ChunkRange &chunk, size_t i,
                         ParallelBatch &parallel,
                         x23::Vec<x23::XdrVec<std::uint8_t>> &xdrs,
                         x23::span<T> &objs) noexcept -> x23::Async<void> {
      co_await default_coropoll().pool_.bg();

      parallel.segment_tokens[i] = chunk.end - chunk.start;
      // This is just an educated guess, but hopefully it lowers/eliminates the
      // resize overhead of the vector.
      xdrs[i].reserve(parallel.segment_tokens[i] * sizeof(T) * 3);
      for (size_t j = chunk.start; j < chunk.end; j++) {
        xdrs[i] += x23::serialize(objs[j]);
      }
      parallel.segment_bytes[i] = xdrs[i].size();
      co_return;
    }(chunk, i, parallel, xdrs, objs));
  }
  // This is just an educated guess, but hopefully it lowers/eliminates the
  // resize overhead of the vector.
  parallel.xdr.reserve(objs.size() * sizeof(T) * 3);
  for (size_t i = 0; i < chunks.size(); i++) {
    co_await handles[i];
    parallel.xdr += xdrs[i];
  }
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

struct ByteArrayHasher {
  template<std::size_t N>
  std::size_t operator()(const std::array<uint8_t, N> &array) const noexcept
  {
    return std::hash<std::string_view>{}(
        std::string_view(reinterpret_cast<const char *>(array.data()), N));
  }
};

} // namespace hogd

// Format ranges is not supported until gcc 15.2
#if (__cpp_lib_format_ranges < 202207L)

#include "hogd/base/string.h"
#include <format>

template<size_t N> struct std::formatter<std::array<uint8_t, N>> {
  static constexpr auto parse(std::format_parse_context &ctx)
  {
    return ctx.begin();
  }
  static auto format(const std::array<uint8_t, N> &array,
                     std::format_context &ctx)
  {
    return std::format_to(ctx.out(), "{}",
                          hogd::encode_hex(std::as_bytes(std::span(array))));
  }
};

#endif

template<> struct std::hash<hogd::AddressAndKey> {
  std::size_t operator()(const hogd::AddressAndKey &addrkey) const noexcept
  {
    auto addr_view =
        std::string_view(reinterpret_cast<const char *>(addrkey.address.data()),
                         addrkey.address.size());
    auto key_disc = addrkey.key.key_type();
    std::string_view key_view = [&] {
      switch (key_disc) {
      case hogd::StorageKeyType::ContractData:
        return std::string_view(reinterpret_cast<const char *>(
                                    addrkey.key.contract_data_key().data()),
                                addrkey.key.contract_data_key().size());
      case hogd::StorageKeyType::FreeNativeTokenBalance:
      case hogd::StorageKeyType::AuthData:
      case hogd::StorageKeyType::Metadata:
      case hogd::StorageKeyType::ReplayCache:
        return std::string_view();
      }
      hogd::panic("unhandled StorageKeyType: {}", key_disc);
    }();

    // This is the hash combine used in boost
    // https://www.boost.org/doc/libs/1_55_0/doc/html/hash/reference.html#boost.hash_combine
    auto hash_combine = []<typename T>(std::size_t &seed, T view) {
      std::hash<T> hasher;
      seed ^= hasher(view) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    };

    std::size_t seed = 0;
    hash_combine(seed, addr_view);
    hash_combine(seed, key_disc);
    hash_combine(seed, key_view);

    return seed;
  }
};
