#pragma once

#include <map>
#include <cstdint>

#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/call_frame.h"
#include "syscall.hh"
#include "transaction.hh"
#include "storage_object.hh"
#include "types.hh"

namespace hogd {

class StorageSnapshotCache;

class SystemCallHandler {
public:

  SystemCallHandler(SignedTransaction signed_tx, StorageSnapshotCache& snapshot_cache);

  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke(Invocation invocation);

  // syscall methods: can only be called when call_frame_stack_ is nonempty
  // (i.e. while at least one invoke() is running)
  ContractAddress get_current_address() const;

  SyscallStatus<ContractAddress> get_msg_sender() const;

  SyscallStatus<const Bytestring> get_bytestring(StorageKey const& key);
  SyscallStatus<uint64_t> get_bytestring_len(StorageKey const& key);

private:

  struct RuntimeCacheEntry {
    std::unique_ptr<WasmiRuntime> runtime;
    bool unwind_poisoned = false;
  };

  WasmiRuntime*
  resolve_runtime(ContractAddress const& addr);

  // Can only be called when call_frame_stack_ is nonempty
  StorageProxy& resolve_storage_proxy(StorageKey const& key);

  SignedTransaction signed_tx_;
  WasmiContext wasmi_context_;
  std::vector<CallFrame> call_frame_stack_;
  std::map<ContractAddress, RuntimeCacheEntry> runtimes_;

  StorageSnapshotCache& snapshot_cache_;
};

} // namespace hogd
