#pragma once

#include <bit>
#include <cassert>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

// Wrapper around a void* that represents an InstancePre from Rust
// The dtor calls free_instance_pre_from_cache to properly clean up
class ExternalInstancePreType {
public:
  // Invariant: instance_pre_ptr is not nullptr, and instance_pre_ptr comes with
  // an associated owned strong reference to the underlying Arc<InstancePre>
  explicit ExternalInstancePreType(void *instance_pre_ptr)
    : instance_pre_ptr_(instance_pre_ptr)
  {
    assert(instance_pre_ptr_ != nullptr);
  }

  ~ExternalInstancePreType();

  ExternalInstancePreType(ExternalInstancePreType const &);
  ExternalInstancePreType &operator=(ExternalInstancePreType const &) = delete;

  // Movable
  ExternalInstancePreType(ExternalInstancePreType &&other) noexcept;
  ExternalInstancePreType &operator=(ExternalInstancePreType &&other) noexcept;

  void *release()
  {
    void *ptr = instance_pre_ptr_;
    instance_pre_ptr_ = nullptr;
    return ptr;
  }

private:
  void *instance_pre_ptr_;
};

class WasmtimeInstanceCache {
public:
  WasmtimeInstanceCache(size_t bucket_count = scalable_shard_size())
    : locks_(bucket_count), buckets_(bucket_count)
  {}

  void put(const uint8_t (*hash)[32], ExternalInstancePreType instance_pre)
  {
    SHA256Hash sha256_hash;
    std::memcpy(sha256_hash.data(), hash, 32);
    auto [ul, map] = get_bucket(sha256_hash);
    map.emplace(sha256_hash, instance_pre);
  }

  std::optional<ExternalInstancePreType> get(const uint8_t (*hash)[32])
  {
    auto span = std::span<const uint8_t, 32>(reinterpret_cast<const uint8_t *>(hash), 32);
    auto [ul, map] = get_bucket(span);
    auto it = map.find(span);
    if (it == map.end()) {
      return std::nullopt;
    }
    return it->second;
  }

private:
  struct KeyEquals {
    using is_transparent = void;

    bool operator()(const SHA256Hash &a, std::span<const uint8_t, 32> b) const
    {
      return std::memcmp(a.data(), b.data(), 32) == 0;
    }

    bool operator()(std::span<const uint8_t, 32> a, const SHA256Hash &b) const
    {
      return std::memcmp(a.data(), b.data(), 32) == 0;
    }

    bool operator()(const SHA256Hash &a, const SHA256Hash &b) const
    {
      return a == b;
    }
  };

  struct KeyHash {
    using is_transparent = void;

    size_t operator()(const SHA256Hash &a) const
    {
      return SipHash24::get_default()(std::as_bytes(std::span(a.data(), 32)));
    }

    size_t operator()(std::span<const uint8_t, 32> b) const
    {
      return SipHash24::get_default()(std::as_bytes(b));
    }
  };

  using Map = std::unordered_map<SHA256Hash, ExternalInstancePreType, KeyHash,
                                 KeyEquals>;

  std::vector<std::mutex> locks_;
  std::vector<Map> buckets_;

  std::pair<std::unique_lock<std::mutex>, Map &>
  get_bucket(std::span<const uint8_t, 32> hash)
  {
    assert(std::has_single_bit(locks_.size()));
    assert(locks_.size() == buckets_.size());

    size_t idx =
        SipHash24::get_default()(std::as_bytes(hash)) & (buckets_.size() - 1);
    return {std::unique_lock<std::mutex>(locks_[idx]), buckets_[idx]};
  }

  std::pair<std::unique_lock<std::mutex>, Map &>
  get_bucket(const SHA256Hash &hash)
  {
    return get_bucket(std::span<const uint8_t, 32>(hash.data(), 32));
  }
};

} // namespace hogd
