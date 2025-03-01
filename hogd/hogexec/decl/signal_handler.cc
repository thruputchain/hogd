#include "hogd/hogexec/decl/signal_handler.h"

#include <cassert>
#include <cstddef>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "lfi_small.h"

namespace hogd {

namespace {

thread_local bool decl_in_syscall = false;

#if defined(__aarch64__)

void signal_trap_handler(int sig, siginfo_t* /*unused*/, void* /*unused*/) {

    if (lfi_proc() && !decl_in_syscall)
        lfi_proc_exit(sig);

    perror("Terminating: signal and no active LFI process");
    // unreachable
    std::abort();
}

void signal_register(int sig) {
    struct sigaction act;
    act.sa_sigaction = &signal_trap_handler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&act.sa_mask);
    if (sigaction(sig, &act, NULL) != 0) {
        perror("sigaction");
        std::abort();
    }
}

thread_local bool sighandler_initialized = false;

}

void decl_mark_inside_syscall() {
    assert(!decl_in_syscall);
    decl_in_syscall = true;
}
void decl_unmark_inside_syscall() {
    assert(decl_in_syscall);
    decl_in_syscall = false;
}

void decl_signal_init() {
	if (sighandler_initialized) return;

    stack_t ss;
    ss.ss_sp = malloc(SIGSTKSZ);
    if (ss.ss_sp == NULL) {
        perror("malloc");
        std::abort();
    }
    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) == -1) {
        perror("sigaltstack");
        std::abort();
    }

    signal_register(SIGSEGV);
    signal_register(SIGILL);
    signal_register(SIGTRAP);
    signal_register(SIGFPE);
    signal_register(SIGBUS);
    sighandler_initialized = true;
}

#else

void decl_signal_init() {
	throw std::runtime_error("wrong architecture");
}

#endif

}