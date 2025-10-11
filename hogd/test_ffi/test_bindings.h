#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Translation of InvokeStatus<void> into an FFI-safe type.
// This is the type returned by an invocation to a mocked contract.
struct ForeignInvokeResult {
  int32_t panic; // if panic is InvokeErrorCode::kInvokeNone, this is translated
                 // to an InvokeStatus<void>{}
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
                                                        uint64_t method_number);

// Translation of HostFnStatus<SyscallErrorCode> into an FFI-safe type.
// TODO(geoff): This is identical to TrampolineResult in runtime_trampoline.
// This could be replaced by the same type as there, although that could
// somewhat complicate the build system/bindgen usage.
struct ForeignHostFnResult {
  int32_t result_code;
  int32_t panic; // HostFnErrorCode
};

// Translation of try_commit_transaction result into an FFI-safe type.
struct ForeignTryCommitResult {
  int32_t result_code; // boolean
  int32_t panic;       // SystemErrorCode
};

void *create_db();
void free_db(void *ptr);

void advance_new_block(void *db);

void *open_runtime_context();
void close_runtime_context(void *context);

// Returns 0 for success, nonzero for some error code
// These errors should not be handled by a contract,
// but are SDK or usage errors (i.e. invalid context pointer)
//
// Note that this does NOT cause the contract to be "created" within the
// database. That still requires some syscall (or setup) that creates a
// Contract(ContractType::kMockTest, id) within the database.  If that creation
// is performed without calling this method, instantiating the contract will
// fail.
int register_new_contract(void *context, int32_t mock_contract_id,
                          ForeignEntryPoint entry_point, void *foreign_context);

// Methods to assist in setup for a test.
// Note that methods for implementing view isolation of one block from a
// sibling block are not implemented; thus, these setup methods just write to
// whatever state is in the database.
// TODO(geoff): Update these methods, and the appropriate parts of the test sdk,
// when that code is merged into the db.
//
// setup_create_contract creates a contract that is _visible_ in the current
// block. Returns 0 on success, 1 otherwise.
int setup_create_contract(void *context, void *db,
                          uint8_t const *contract_bytes, uint32_t contract_len);
// setup_create_contract creates a contract that is _visible_ in the current
// block.
int setup_deploy_contract(void *db, const uint8_t *deployment_addr,
                          const uint8_t *contract_hash, uint32_t contract_size);

void setup_grant_contract_storage_tokens(void *db, const uint8_t *contract_addr,
                                         int64_t tokens);

// db must have been returned by create_db()
// rc must have been returned by open_runtime_context()
void *open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx,
                           unsigned int signed_tx_len);
void close_syscall_handler(void *syscall_handler);

// Get the event log for the current transaction.
// Returns 0 on success, 1 otherwise.
// syscall_handler must have been returned by open_syscall_handler()
// buffer must be of the proper size for the event log for the tx (query from
// get_event_log_len)
int get_event_log(void *syscall_handler, unsigned char *buffer,
                  uint32_t buffer_len);
uint32_t get_event_log_len(void *syscall_handler);

struct ForeignHostFnResult mock_syscall_entrypoint(int32_t syscall,
                                                   uint64_t arg1, uint64_t arg2,
                                                   uint64_t arg3, uint64_t arg4,
                                                   uint64_t arg5, uint64_t arg6,
                                                   uint64_t arg7);

struct ForeignRootInvokeResult root_invoke(void *syscall_handler);
struct ForeignTryCommitResult try_commit_transaction(void *syscall_handler);

void enable_syscall_logging();

#ifdef __cplusplus
}
#endif
