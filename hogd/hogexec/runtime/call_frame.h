#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <vector>

#include "hogd/hogexec/runtime/item_cache.h"
#include "storage_object.hh"
#include "transaction.hh"

namespace hogd {

class CallFrame {
public:
  CallFrame(Invocation invocation);

  void commit_successful_child_call(CallFrame &frame);
  // revert the child call frame
  void commit_failed_child_call();

  ItemCache *try_get_item_cache(AddressAndKey const &query);

  void push_new_item_cache(AddressAndKey const &new_addrkey,
                           ItemCache const &proxy);

  // The set of all runtimes visited by the current call frame and _successful_
  // children. Failed calls poison the set of visited runtimes, unless the
  // runtime is marked UNWIND_SAFE (not this PR) Failed call frames immediately
  // poison their set of visited runtimes.
  std::set<ContractAddress> visited_runtimes_;

  // The value of the last _child_ call frame's returned string.
  // Set to nullopt on failed calls, or before any child call happens.
  std::optional<ReturnBuffer> child_return_buffer_ = std::nullopt;

  void set_call_return(ReturnBuffer &&return_buffer);
  void log(std::span<const std::byte> message);

  std::vector<LogEntry> extract_logs() { return std::move(logs_); }

  ItemCacheMap &get_item_cache_map() { return item_cache_map_; }

  const Invocation active_invocation_;

  std::map<ContractAddress, SHA256Hash> new_contract_deployments_;

private:
  ReturnBuffer to_return_;

  // Cached here separately from the primary SystemCallHandler
  // in order to (in later PR) support subcall unwinding
  // swap to non unique_ptr
  ItemCacheMap item_cache_map_;

  std::vector<LogEntry> logs_;
};

} // namespace hogd
