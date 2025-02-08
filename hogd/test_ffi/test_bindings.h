#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct ForeignInvokeResult {
  int32_t result_code;
  int32_t panic;
};

struct ForeignRootInvokeResult {
  struct ForeignInvokeResult result;
  uint64_t gas_consumed;
};

// NOLINTNEXTLINE(modernize-use-using)
typedef struct ForeignInvokeResult (*ForeignEntryPoint)(void *foreign_context,
                                                        uint32_t method_number);

void *create_db();
void free_db(void *ptr);

void advance_new_block(void *db);

void *open_runtime_context();
void close_runtime_context(void *context);

// Returns 0 for success, nonzero for some error code
// entrypoint should be of type detail::ForeignEntryPoint
int add_contract_and_method(void *context, const unsigned char *contract_hash,
                            ForeignEntryPoint entry_point,
                            void *foreign_context);

void *open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx_,
                           unsigned int serial_len);
void close_syscall_handler(void *syscall_handler);

// returns an FFIRootInvokeResult
struct ForeignRootInvokeResult root_invoke(void *syscall_handler);

#ifdef __cplusplus
}
#endif
