#include "hogd/hogexec/decl/lficpp.h"

#include <cassert>
#include <memory>
#include <mutex>

#include "hogd/base/types.h"
#include "lfi_small.h"

namespace hogd {

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
}


DeclProc::~DeclProc() {
    main_engine_.delete_proc(proc_);
}

}