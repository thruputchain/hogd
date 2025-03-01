#pragma once

namespace hogd
{

// This function should be called by each thread that uses DeCl
// Second call from a thread is a no-op
void decl_signal_init();

// Mark whether or not a decl invocation is currently running within a syscall.
// We do not want e.g. a segfault inside a syscall (which is a groundhog error,
// not a contract error) to inadvertently cause a contract error.  This might
// introduce, for example, nondeterminism in a hard-to-catch way.
void decl_mark_inside_syscall();
void decl_unmark_inside_syscall();

}