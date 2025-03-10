#pragma once

#include <memory>
#include <mutex>

#include "hogd/base/types.h"
#include "lfi_small.h"

namespace hogd {

class DeclProc;

// Process should create one of these engines.
// This struct is threadsafe (everything is guarded by a mutex)
// and simply allocates new processes as desired.
// Transaction should not be talking to this engine directly,
// but rather using local caches on LFIProcs to avoid contention
// on this structure.
class LFIGlobalEngine {
public:
  LFIGlobalEngine(SysHandler handler);
  ~LFIGlobalEngine();

  LFIGlobalEngine(LFIGlobalEngine const &) = delete;
  LFIGlobalEngine(LFIGlobalEngine &&) = delete;
  LFIGlobalEngine &operator=(LFIGlobalEngine const &) = delete;
  LFIGlobalEngine &operator=(LFIGlobalEngine &&) = delete;

  // This may fail to create a new proc.
  // That should be considered a nondeterministic error,
  // and not one that is visible to a smart contract
  // (i.e. triggering a kUnrecoverable error).
  [[nodiscard]] std::unique_ptr<DeclProc> new_proc();

  void delete_proc(LFIProc *proc);

private:
  std::mutex mtx_;
  LFIEngine *lfi_engine_ = nullptr;
  uint32_t active_proc_count_ = 0;
};

class DeclProc {
public:
  DeclProc(DeclProc const &) = delete;
  DeclProc(DeclProc &&) = delete;
  DeclProc operator=(DeclProc const &) = delete;
  DeclProc operator=(DeclProc &&) = delete;

  ~DeclProc();

private:
  friend class LFIGlobalEngine;

  // two-part construction.  LFIGlobalEngine creates a DeclProc
  // object, then creates an LFIProc* with the DeclProc* as the context,
  // then sets proc_.
  DeclProc(LFIGlobalEngine &engine) : main_engine_(engine) {}

  LFIGlobalEngine &main_engine_;
  LFIProc *proc_ = nullptr;
};

} // namespace hogd
