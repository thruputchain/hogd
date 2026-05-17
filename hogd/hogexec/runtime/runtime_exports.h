#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct TrampolineResult {
  int32_t result_code;
  int32_t panic; // HostFnErrorCode
};

struct LinearMemorySlice {
  uint8_t *mem;
  uint64_t sz;
};

// The first parameter is the host call context.  This is a c++ only type, and
// passed around within the rust as a void*.

// NOLINTBEGIN(modernize-use-using)

typedef void *ExternHostCallContext;

typedef struct TrampolineResult (*HostFn0)(ExternHostCallContext,
                                           struct LinearMemorySlice);
typedef struct TrampolineResult (*HostFn1)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t);
typedef struct TrampolineResult (*HostFn2)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t);
typedef struct TrampolineResult (*HostFn3)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t);
typedef struct TrampolineResult (*HostFn4)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t, uint64_t);
typedef struct TrampolineResult (*HostFn5)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t);
typedef struct TrampolineResult (*HostFn6)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t);
typedef struct TrampolineResult (*HostFn7)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t, uint64_t);
typedef struct TrampolineResult (*HostFn8)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t);
typedef struct TrampolineResult (*HostFn9)(ExternHostCallContext,
                                           struct LinearMemorySlice, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t, uint64_t,
                                           uint64_t, uint64_t);
typedef struct TrampolineResult (*HostFn10)(ExternHostCallContext,
                                            struct LinearMemorySlice, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t);
typedef struct TrampolineResult (*HostFn11)(ExternHostCallContext,
                                            struct LinearMemorySlice, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t);
typedef struct TrampolineResult (*HostFn12)(ExternHostCallContext,
                                            struct LinearMemorySlice, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t, uint64_t,
                                            uint64_t, uint64_t);

// NOLINTEND(modernize-use-using)

#ifdef __cplusplus
} // extern "C"
#endif
