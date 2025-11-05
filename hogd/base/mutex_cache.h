#pragma once

#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

#include "hogd/base/os.h"
#include "hogd/base/types.h"

namespace hogd {

template<typename Hash> class MutexCache {
public:
  MutexCache(size_t n = scalable_shard_size()) : mutexes_(n) {}

  std::mutex &get_mutex(std::span<const std::byte> key)
  {
    static Hash hash;
    return mutexes_[hash(key) % mutexes_.size()];
  }

private:
  std::vector<std::mutex> mutexes_;
};

} // namespace hogd
