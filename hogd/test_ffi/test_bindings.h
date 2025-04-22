#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Translation of InvokeStatus<int32_t> into an FFI-safe type.
// This is the type returned by an invocation to a mocked contract.
struct ForeignInvokeResult {
  int32_t result_code;
  int32_t panic;
};

// FFI-safe type corresponding to the result of a root invocation.
// I.e. InvokeStatus<int32_t> plus the gas consumed.
struct ForeignRootInvokeResult {
  struct ForeignInvokeResult result;
  uint64_t gas_consumed;
};

// Type of the function pointer used within a mock contract.
// NOLINTNEXTLINE(modernize-use-using)
typedef struct ForeignInvokeResult (*ForeignEntryPoint)(void *foreign_context,
                                                        uint32_t method_number);

// Translation of HostFnStatus<SyscallErrorCode> into an FFI-safe type.
// TODO(geoff): This is identical to TrampolineResult in runtime_trampoline.
// This could be replaced by the same type as there, although that could
// somewhat complicate the build system/bindgen usage.
struct ForeignHostFnResult {
  int32_t result_code;
  int32_t panic; // HostFnErrorCode
};

void *create_db();
void free_db(void *ptr);

void advance_new_block(void *db);

void *open_runtime_context();
void close_runtime_context(void *context);

// Returns 0 for success, nonzero for some error code
int add_contract_and_method(void *context, uint32_t mock_contract_id,
                            ForeignEntryPoint entry_point,
                            void *foreign_context);

void *open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                           unsigned int signed_tx_len);
void close_syscall_handler(void *syscall_handler);

struct ForeignHostFnResult mock_syscall_entrypoint(uint64_t syscall,
                                                   uint64_t arg1, uint64_t arg2,
                                                   uint64_t arg3, uint64_t arg4,
                                                   uint64_t arg5,
                                                   uint64_t arg6);

struct ForeignRootInvokeResult root_invoke(void *syscall_handler);

#ifdef __cplusplus
}
#endif
