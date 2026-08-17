#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct TrampolineResult {
  int32_t result_code;
  int32_t panic; // HostFnErrorCode
  uint64_t gas_consumed;
};

struct LinearMemorySlice {
  uint8_t *mem;
  uint64_t sz;
};

struct WasmContext {
  struct LinearMemorySlice memory;
  uint64_t gas_remaining;
};

// The first parameter is the host call context: a C++ `SystemCallHandler*`
// installed by the host when the sandbox is created and handed back, unchanged,
// to syscall_entry on every host call. Rust treats it as an opaque void*.

// NOLINTBEGIN(modernize-use-using)

typedef void *ExternHostCallContext;

typedef struct TrampolineResult (*HostFn8)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t);


// NOLINTEND(modernize-use-using)

#ifdef __cplusplus
} // extern "C"
#endif
