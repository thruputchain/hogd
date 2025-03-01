#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "hogd/hogexec/decl/lficpp.h"

namespace hogd {

class SandboxCache {
  public:
    SandboxCache(LFIGlobalEngine& global_engine);

    std::unique_ptr<DeclProc> get_new_proc();
    void return_proc(std::unique_ptr<DeclProc> proc);

private:
    std::vector<std::unique_ptr<DeclProc>> free_procs_;
    LFIGlobalEngine& global_engine_;
};

} // namespace scs