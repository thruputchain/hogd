#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hogd {

// Owns bytes returned by wasm_meter_instrument(). Move-only.
class InstrumentedBytes {
public:
  // Returns nullopt on instrumentation failure.
  static std::optional<InstrumentedBytes>
  instrument(std::span<const uint8_t> wasm);

  std::span<const uint8_t> data() const;

  ~InstrumentedBytes();
  InstrumentedBytes(InstrumentedBytes &&other) noexcept;
  InstrumentedBytes &operator=(InstrumentedBytes &&other) noexcept;
  InstrumentedBytes(const InstrumentedBytes &) = delete;
  InstrumentedBytes &operator=(const InstrumentedBytes &) = delete;

private:
  InstrumentedBytes(uint8_t *data, size_t len);
  uint8_t *data_ = nullptr;
  size_t len_ = 0;
};

} // namespace hogd
