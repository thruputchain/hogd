#pragma once

extern "C" {

void *create_db();
void free_db(void *ptr);

void* open_syscall_handler(void *db);
void close_syscall_handler(void* syscall_handler);
}
