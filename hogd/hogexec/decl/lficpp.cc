#include "hogd/hogexec/decl/lficpp.h"

#include <cassert>
#include <memory>
#include <mutex>

#include "hogd/base/types.h"
#include "lfi_small.h"

namespace hogd {

LFIGlobalEngine::LFIGlobalEngine(SysHandler handler)
{
  LFIOptions options{
      .noverify = true,
      .poc = true,
      .sysexternal = true,
      .p2size = 0,
      .pagesize = 4 * 1024,
      .stacksize = 65536,
      .gas = 0,
      .verifier = nullptr,
      .syshandler = handler,
  };

  lfi_engine_ = lfi_new(options);
  assert(lfi_engine_);
}

LFIGlobalEngine::~LFIGlobalEngine()
{
  assert(active_proc_count_ == 0);
  if (lfi_engine_) {
    lfi_delete(lfi_engine_);
  }
  lfi_engine_ = nullptr;
}

std::unique_ptr<DeclProc>
LFIGlobalEngine::new_proc()
{
  std::lock_guard lock(mtx_);

  auto *decl_proc = new DeclProc(*this);

  assert(!decl_proc->proc_);

  auto res =
      lfi_adduproc(lfi_engine_, &decl_proc->proc_,
                   static_cast<void *>(decl_proc), 0x20000, 128_KiB, 128_KiB);

  if (!res) {
    delete decl_proc;
    return nullptr;
  }
  active_proc_count_++;

  return std::unique_ptr<DeclProc>(decl_proc);
}

void
LFIGlobalEngine::delete_proc(LFIProc *proc)
{
  if (!proc) {
    return;
  }

  std::lock_guard lock(mtx_);
  active_proc_count_--;
  lfi_rmproc(lfi_engine_, proc);
}

DeclProc::~DeclProc()
{
  main_engine_.delete_proc(proc_);
}

} // namespace hogd
