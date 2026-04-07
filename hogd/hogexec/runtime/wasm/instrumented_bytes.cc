#include "hogd/hogexec/runtime/wasm/instrumented_bytes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "hogd/rust/wasm_meter/bindings.h"

namespace hogd {

InstrumentedBytes::InstrumentedBytes(uint8_t *data, size_t len)
  : data_(data), len_(len)
{}

InstrumentedBytes::~InstrumentedBytes()
{
  if (data_ != nullptr) {
    wasm_meter_free(data_, len_);
  }
}

InstrumentedBytes::InstrumentedBytes(InstrumentedBytes &&other) noexcept
  : data_(std::exchange(other.data_, nullptr)),
    len_(std::exchange(other.len_, 0))
{}

InstrumentedBytes &
InstrumentedBytes::operator=(InstrumentedBytes &&other) noexcept
{
  if (this != &other) {
    if (data_ != nullptr) {
      wasm_meter_free(data_, len_);
    }
    data_ = std::exchange(other.data_, nullptr);
    len_ = std::exchange(other.len_, 0);
  }
  return *this;
}

std::optional<InstrumentedBytes>
InstrumentedBytes::instrument(std::span<const uint8_t> wasm)
{
  auto result = wasm_meter_instrument(wasm.data(), wasm.size());
  if (!result.success) {
    return std::nullopt;
  }
  return InstrumentedBytes(result.data, result.len);
}

std::span<const uint8_t>
InstrumentedBytes::data() const
{
  return {data_, len_};
}

} // namespace hogd
