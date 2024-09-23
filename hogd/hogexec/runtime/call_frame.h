#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>

#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_object.hh"
#include "transaction.hh"

namespace hogd {

class CallFrame {
public:
  CallFrame(ContractAddress const &addr, WasmiRuntime &runtime);

  void commit_successful_child_call(CallFrame &&frame);

  const bool unwind_catch_boundary_ = false;
  const ContractAddress active_address_;

  StorageProxy *try_get_storage_proxy(AddressAndKey const &query);

  void push_new_storage_proxy(AddressAndKey const &new_addrkey,
                              StorageProxy const &proxy);

  // The set of all runtimes visited by the current call frame and _successful_
  // children. Failed calls poison the set of visited runtimes, unless the
  // runtime is marked UNWIND_SAFE (not this PR) Failed call frames immediately
  // poison their set of visited runtimes, so we do not need to update this
  // variable here.
  std::set<ContractAddress> visited_runtimes_;

  ReturnBuffer last_return_buffer;

private:
  WasmiRuntime &active_runtime_;

  // Cached here separately from the primary SystemCallHandler
  // in order to (in later PR) support subcall unwinding
  std::map<AddressAndKey, std::unique_ptr<StorageProxy>> storage_proxies_;
};

} // namespace hogd
