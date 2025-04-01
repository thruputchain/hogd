#pragma once

#include <atomic>
#include <cstdint>

namespace hogd {

class PortAllocator {
public:
  static uint32_t get() { return port_.fetch_add(1); }

private:
  inline static std::atomic<uint32_t> port_ = 10000;
};

} // namespace hogd
