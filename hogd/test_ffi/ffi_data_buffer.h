#pragma once

#include <cstring>
#include <exception>
#include <expected>
#include <format>

#include "hogd/base/types.h"
#include "hogd/test_ffi/ffi_error.h"
#include "hogd/test_ffi/test_bindings.h"
#include "x23/serialize.h"
#include "x23/x23.h"

// Utility methods for working with objects serialized to pass across the FFI
// boundary.

namespace hogd {

constexpr FFIDataBuffer kNullBuffer = FFIDataBuffer{.data = nullptr, .len = 0};

inline std::expected<FFIDataBuffer, TestFFIErrorCode>
make_new_data_buffer(uint64_t len) noexcept
{
  if (len == 0) {
    return FFIDataBuffer{.data = nullptr, .len = 0};
  }
  try {
    auto *data = new uint8_t[len];
    return FFIDataBuffer{.data = data, .len = len};
  } catch (std::exception const &e) {
    log_error("error allocating data buffer: {}", e);
    return std::unexpected(kTestFFIErrorCodeUnrecoverableError);
  }
}

inline TestFFIErrorCode
free_ffi_data_buffer(FFIDataBuffer buffer) noexcept
{
  if (buffer.data == nullptr && buffer.len != 0) {
    return kTestFFIErrorCodeInvalidPointer;
  }
  delete[] buffer.data;
  return kTestFFIErrorCodeNone;
}

template<x23::is_xdr_type T>
std::expected<FFIDataBuffer, TestFFIErrorCode>
serialize_to_ffi_data_buffer(T const &value) noexcept
{
  try {
    auto serialized = x23::serialize(value);
    auto buffer = make_new_data_buffer(serialized.size());
    if (!buffer.has_value()) {
      return std::unexpected(buffer.error());
    }
    std::memcpy(buffer.value().data, serialized.data(), serialized.size());
    return buffer.value();
  } catch (std::exception const &e) {
    log_error("error serializing to ffi data buffer: {}", e);
    return std::unexpected(kTestFFIErrorCodeUnrecoverableError);
  }
}

} // namespace hogd
