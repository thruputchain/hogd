#pragma once

#include <cstdint>

namespace hogd::detail {

extern "C" {

struct ForeignInvokeResult {
  int32_t result_code;
  int32_t panic;
};
}

using ForeignEntryPoint = ForeignInvokeResult (*)();

} // namespace hogd::detail
