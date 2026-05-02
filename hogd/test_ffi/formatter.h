#pragma once

#include <cstdint>
#include <format>
#include <span>
#include <utility>

#include "hogd/base/string.h"
#include "hogd/test_ffi/test_bindings.h"

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

template<> struct formatter<TestFFIErrorCode> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const TestFFIErrorCode &error_code, auto &ctx)
  {
    return format_to(ctx.out(), "TestFFIErrorCode{{{}}}",
                     std::to_underlying(error_code));
  }
};

template<> struct formatter<ForeignTracedInvokeResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const ForeignTracedInvokeResult &result, auto &ctx)
  {
    return format_to(ctx.out(),
                     "serialized_output=[{}], gas_consumed={}, "
                     "serialized_trace=[{}], serialized_events=[{}], "
                     "ffi_error_code={}",
                     result.serialized_output, result.gas_consumed,
                     result.serialized_trace, result.serialized_events,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<ForeignTryCommitResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const ForeignTryCommitResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "result_code={}, ffi_error_code={}",
                     result.result_code,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<CreateDBResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const CreateDBResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "db={}, ffi_error_code={}", result.db,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<GetCostModelResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const GetCostModelResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "data=[{}], ffi_error_code={}", result.data,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<OpenRuntimeContextResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const OpenRuntimeContextResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "context={}, ffi_error_code={}", result.context,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<FFISerializedSignedTransaction> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const FFISerializedSignedTransaction &transaction,
                     auto &ctx)
  {
    return format_to(ctx.out(), "data={}, len={}",
                     static_cast<const void *>(transaction.data),
                     transaction.len);
  }
};

template<> struct formatter<FFISourceAuthData> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const FFISourceAuthData &auth_data, auto &ctx)
  {
    return format_to(ctx.out(), "data={}, len={}",
                     static_cast<const void *>(auth_data.data), auth_data.len);
  }
};

template<> struct formatter<VerifyAuthResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const VerifyAuthResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "auth_error_code={}, ffi_error_code={}",
                     result.auth_error_code,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<VerifyAuthResultFromDBResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const VerifyAuthResultFromDBResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "auth_error_code={}, ffi_error_code={}",
                     result.auth_error_code,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<OpenSyscallHandlerResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const OpenSyscallHandlerResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "syscall_handler={}, ffi_error_code={}",
                     result.syscall_handler,
                     std::to_underlying(result.ffi_error_code));
  }
};

template<> struct formatter<ForeignHostFnResult> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const ForeignHostFnResult &result, auto &ctx)
  {
    return format_to(ctx.out(), "result_code={}, panic={}", result.result_code,
                     result.panic);
  }
};

} // namespace std
