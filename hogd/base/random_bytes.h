// random_bytes.h - utility for generating random bytes for testing

#pragma once

#include <algorithm>
#include <climits>
#include <random>
#include <type_traits>

namespace hogd {

// always use the same seed value to make tests deterministic
constexpr inline uint8_t kRandomSeed = 0xBE;

// RandomBytes generates pseudorandom bytes
class RandomBytes {
public:
  RandomBytes(uint8_t seed = kRandomSeed) : rand_engine_{seed} {}

  template<typename T>
  requires std::is_trivial_v<T> && (sizeof(T) == sizeof(std::byte))
  void operator()(std::span<T> buf)
  {
    std::generate(buf.begin(), buf.end(),
                  [this] { return static_cast<T>(rand_engine_()); });
  }

private:
  using random_bytes_engine =
      std::independent_bits_engine<std::default_random_engine, CHAR_BIT,
                                   uint8_t>;
  random_bytes_engine rand_engine_;
};

} // namespace hogd
