#pragma once

#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <format>
#include <span>

#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/test_ffi/ffi_error.h"
#include "hogd/test_ffi/test_bindings.h"
#include "x23/serialize.h"
#include "x23/x23.h"

// Utility methods for working with objects serialized to pass across the FFI
// boundary.

namespace std {

template<> struct formatter<FFIDataBuffer> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const FFIDataBuffer &buffer, auto &ctx)
  {
    if (buffer.data == nullptr) {
      return format_to(ctx.out(), "FFIDataBuffer{{data=nullptr, len={}}}",
                       buffer.len);
    }
    if (buffer.len == 0) {
      return format_to(ctx.out(), "FFIDataBuffer{{data=empty, len=0}}");
    }
    auto hex = hogd::encode_hex(
        std::as_bytes(std::span<const uint8_t>(buffer.data, buffer.len)));
    return format_to(ctx.out(), "FFIDataBuffer{{data={}, len={}}}", hex,
                     buffer.len);
  }
};
} // namespace std

namespace hogd {

constexpr FFIDataBuffer kNullBuffer = FFIDataBuffer{.data = nullptr, .len = 0};

inline std::expected<FFIDataBuffer, TestFFIErrorCode>
make_new_data_buffer(uint32_t len) noexcept
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
  auto serialized = x23::serialize(value);
  auto buffer = make_new_data_buffer(serialized.size());
  if (!buffer.has_value()) {
    return std::unexpected(buffer.error());
  }
  std::memcpy(buffer.value().data, serialized.data(), serialized.size());
  return buffer.value();
}

} // namespace hogd
