#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>

#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_object.hh"
#include "transaction.hh"

namespace hogd {

class CallFrame {
public:
  CallFrame(Invocation invocation, WasmiRuntime &runtime);

  CallFrame(CallFrame &&other) = default;

  void commit_successful_child_call(CallFrame &&frame);
  void commit_failed_child_call();

  // TODO(geoff) set based on metadata
  const bool unwind_catch_boundary_ = false;
  const Invocation active_invocation_;

  StorageProxy *try_get_storage_proxy(AddressAndKey const &query);

  void push_new_storage_proxy(AddressAndKey const &new_addrkey,
                              StorageProxy const &proxy);

  // The set of all runtimes visited by the current call frame and _successful_
  // children. Failed calls poison the set of visited runtimes, unless the
  // runtime is marked UNWIND_SAFE (not this PR) Failed call frames immediately
  // poison their set of visited runtimes, so we do not need to update this
  // variable here.
  std::set<ContractAddress> visited_runtimes_;

  // The value of the last _child_ call frame's returned string.
  // Set to nullopt on failed calls, or before any child call happens.
  std::optional<ReturnBuffer> child_return_buffer_ = std::nullopt;

  void set_call_return(ReturnBuffer&& return_buffer);
  void log(std::span<const std::byte> message);

  std::vector<LogEntry> extract_logs() { return std::move(logs_); }

  uint64_t get_available_gas() { return active_runtime_.get_available_gas(); }

private:
  WasmiRuntime &active_runtime_;

  ReturnBuffer to_return_;

  // Cached here separately from the primary SystemCallHandler
  // in order to (in later PR) support subcall unwinding
  std::map<AddressAndKey, std::unique_ptr<StorageProxy>> storage_proxies_;

  std::vector<LogEntry> logs_;
};

} // namespace hogd
