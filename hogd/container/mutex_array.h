#pragma once


#include <bit>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <vector>

#include "hogd/base/os.h"
#include "hogd/base/types.h"

namespace hogd {

class MutexArray {
public:
    MutexArray(size_t size = scalable_shard_size()) : mutexes_(size) {
        assert(std::has_single_bit(size));
    }

    std::mutex& get_mutex(uint32_t shorthash) {
        size_t idx = shorthash & (mutexes_.size() - 1);
        return mutexes_[idx];
    }

private:
    std::vector<std::mutex> mutexes_;
};
}