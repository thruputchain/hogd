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

// The first parameter is the host call context.  This is a c++ only type, and
// passed around within the rust as a void*.

// NOLINTBEGIN(modernize-use-using)

typedef void *ExternHostCallContext;

// wasm32 syscall ABI: each argument slot is a 32-bit value (i32). Note that
// WasmContext.gas_remaining and LinearMemorySlice.sz remain uint64_t; they are
// not argument slots.
typedef struct TrampolineResult (*HostFn0)(ExternHostCallContext,
                                           struct WasmContext);
typedef struct TrampolineResult (*HostFn1)(ExternHostCallContext,
                                           struct WasmContext, uint32_t);
typedef struct TrampolineResult (*HostFn2)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t);
typedef struct TrampolineResult (*HostFn3)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t);
typedef struct TrampolineResult (*HostFn4)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t);
typedef struct TrampolineResult (*HostFn5)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t);
typedef struct TrampolineResult (*HostFn6)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t);
typedef struct TrampolineResult (*HostFn7)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t);
typedef struct TrampolineResult (*HostFn8)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t);
typedef struct TrampolineResult (*HostFn9)(ExternHostCallContext,
                                           struct WasmContext, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t);
typedef struct TrampolineResult (*HostFn10)(ExternHostCallContext,
                                            struct WasmContext, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t);
typedef struct TrampolineResult (*HostFn11)(ExternHostCallContext,
                                            struct WasmContext, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t);
typedef struct TrampolineResult (*HostFn12)(ExternHostCallContext,
                                            struct WasmContext, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t, uint32_t,
                                            uint32_t, uint32_t);

// NOLINTEND(modernize-use-using)

#ifdef __cplusplus
} // extern "C"
#endif
