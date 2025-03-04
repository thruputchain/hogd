#include "hogd/hogexec/decl/lficpp.h"

#include <cassert>
#include <memory>
#include <mutex>

#include "contract.hh"
#include "hogd/base/types.h"
#include "lfi_small.h"

namespace hogd {

LFIGlobalEngine::LFIGlobalEngine(SysHandler handler)
{
    LFIOptions opts = (LFIOptions) {
        .noverify = true, // programs should be verified when they're registered, not at runtime
        .poc = true,
        .sysexternal = true,
        .pagesize = 16 * 1024,
        .stacksize = 65536, 
        .gas = 0,
        .syshandler = handler,
    };

    lfi_engine_ = lfi_new(opts);
    assert(lfi_engine_);

    [[maybe_unused]]
    bool ok = lfi_reserve(lfi_engine_,  8ULL * 1024_GiB); // 8TB
    assert(ok);
}

LFIGlobalEngine::~LFIGlobalEngine()
{
    lfi_delete(lfi_engine_);
    assert(active_proc_count_ == 0);
}

std::unique_ptr<DeclProc>
LFIGlobalEngine::new_proc()
{
    std::lock_guard lock(mtx_);
    
    auto* decl_proc = new DeclProc(*this);

    assert(!decl_proc->proc_);

    auto res = lfi_adduproc(lfi_engine_, &decl_proc->proc_, static_cast<void*>(decl_proc), 0x20000, 128_KiB, 128_KiB);

    if (!res) {
        delete decl_proc;
        return nullptr;
    }
    active_proc_count_++;
    return std::unique_ptr<DeclProc>(decl_proc);
}

void 
LFIGlobalEngine::delete_proc(LFIProc* proc)
{
    if (!proc) {
        return;
    }

    std::lock_guard lock(mtx_);
    lfi_rmproc(lfi_engine_, proc);
    active_proc_count_--;
}


DeclProc::~DeclProc() {
    main_engine_.delete_proc(proc_);
}

namespace {

void
verifier_errfn(char* msg, size_t /*unused*/)
{
    std::println(stderr, "%s\n", msg);
}

}

bool verify_contract(Contract const* contract) {
    if (!contract) return false;

    #if defined(__aarch64__)
        

    #elif defined(__x86_64__)

    #else
        return false;
    #endif
}

}