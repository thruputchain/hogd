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
// entrypoint should be of type detail::ForeignEntryPoint
int add_contract_and_method(void *context, const unsigned char *contract_hash,
                            ForeignEntryPoint entry_point,
                            void *foreign_context);

// registers a test contract as created within the root database
void setup_create_contract(void *db, const unsigned char *contract_data,
                           uint32_t contract_len);

// registers a deployment within the root database
void setup_create_deployment(void *db, const unsigned char *deployment_addr,
                             const unsigned char *deployed_hash);

void *open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx_,
                           unsigned int serial_len);
void close_syscall_handler(void *syscall_handler);

struct ForeignHostFnResult mock_syscall_entrypoint(int32_t syscall,
                                                   const unsigned char *input,
                                                   uint32_t input_len,
                                                   unsigned char *output,
                                                   uint32_t output_len);

// returns an FFIRootInvokeResult
struct ForeignRootInvokeResult root_invoke(void *syscall_handler);

#ifdef __cplusplus
}
#endif
