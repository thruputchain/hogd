#include "hogd/hogexec/decl/sandbox_cache.h"

#include <cassert>
#include <memory>
#include <utility>

#include "hogd/hogexec/decl/lficpp.h"

namespace hogd {

SandboxCache::SandboxCache(LFIGlobalEngine &global_engine)
  : global_engine_(global_engine)
{}

std::unique_ptr<DeclProc>
SandboxCache::get_new_proc()
{
  if (free_procs_.empty()) {
    return global_engine_.new_proc();
  }
  auto back = std::move(free_procs_.back());
  assert(back);
  free_procs_.pop_back();

  return back;
}
void
SandboxCache::return_proc(std::unique_ptr<DeclProc> proc)
{
  free_procs_.emplace_back(std::move(proc));
}

} // namespace hogd
