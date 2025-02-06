#pragma once

#include <cstdint>

extern "C" {

void *create_db();
void free_db(void *ptr);

void *open_runtime_context();
void close_runtime_context(void *context);
void add_contract_and_method(void *context, const uint8_t *contract_hash,
                             const char *method_name, uint8_t method_name_len,
                             void *entry_point);

void *open_syscall_handler(void *db);
void close_syscall_handler(void *syscall_handler);
}
