#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void *create_db();
void free_db(void *ptr);

void *open_runtime_context();
void close_runtime_context(void *context);
// entrypoint should be of type detail::ForeignEntryPoint
void add_contract_and_method(void *context, const unsigned char *contract_hash,
                             const unsigned char *method_name,
                             unsigned int method_name_len, void *entry_point);

void *open_syscall_handler(void *db, void *rc, const unsigned char *signed_tx_,
                           unsigned int serial_len);
void close_syscall_handler(void *syscall_handler);

#ifdef __cplusplus
}
#endif
