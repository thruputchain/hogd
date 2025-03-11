#include "hogd/hogexec/decl/lficpp.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <print>

#include "contract.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "lfi_small.h"

namespace hogd {

LFIGlobalEngine::LFIGlobalEngine(SysHandler handler)
{
    LFIOptions options{
      .noverify = true,
      .poc = true,
      .sysexternal = true,
      .p2size = 0,
      .pagesize = 16 * 1024,
      .stacksize = 65536,
      .gas = 0,
      .verifier = nullptr,
      .syshandler = handler,
  };

  lfi_engine_ = lfi_new(options);
  assert(lfi_engine_);

  [[maybe_unused]]
  bool ok = lfi_reserve(lfi_engine_,  8ULL * 1024_GiB); // 8TB
  assert(ok);
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

bool
DeclProc::load_program(Contract const* contract) {
  assert(!actively_running_);
  assert(contract);

  // TODO(geoff): For unclear reasons (I think no reason at all) loadelf takes a uint8_t* instead of const uint8_t*
  auto* data = const_cast<uint8_t*>(contract->contract_data.data());
  size_t data_len = contract->contract_data.size();

  LFIProcInfo info;

  auto res = lfi_proc_loadelf(proc_, data, data_len, NULL, 0, &info);

  if (!res) {
    active_memory_ = {{
      Runtime::kNullMemorySpan, Runtime::kNullMemorySpan, Runtime::kNullMemorySpan}
    };
    return false;
  }
  
  // NOLINTBEGIN(performance-no-int-to-ptr)
  MemorySpan code_span = {
    .bytes_ = {reinterpret_cast<std::byte*>(lfi_proc_base(proc_) + 0x20000), 128_KiB},
    .base_offset_ = 0x20000,
    .writable_ = false
  };

  MemorySpan data_span = {
    .bytes_ = {reinterpret_cast<std::byte*>(lfi_proc_base(proc_) + 0x20000 + 128_KiB), 128_KiB},
    .base_offset_ = 0x20000 + 128_KiB,
    .writable_ = true
  };

  MemorySpan stack_span = {
    .bytes_ = {reinterpret_cast<std::byte*>(info.stack), info.stacksize},
    .base_offset_ = reinterpret_cast<uintptr_t>(info.stack) - lfi_proc_base(proc_),
    .writable_ = true
  };
  // NOLINTEND(performance-no-int-to-ptr)
  active_memory_ = {
    {code_span, data_span, stack_span}
  };

  return true;
}

Memory 
DeclProc::get_memory()
{
  return active_memory_;
}


namespace {

void
verifier_errfn(char* msg, size_t /*unused*/)
{
    std::println(stderr, "%s", msg);
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
