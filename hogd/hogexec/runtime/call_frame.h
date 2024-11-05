#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>

#include "hogd/hogexec/runtime/auth_manager.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "storage_object.hh"
#include "transaction.hh"

namespace hogd {

enum class CallFrameFlags : uint64_t {
  VerifyAuthFrame = 1,
};

class CallFrame {
public:
  CallFrame(Invocation invocation, AuthManager const &auth_manager,
            uint64_t flags);

  void commit_successful_child_call(CallFrame &frame);
  void commit_failed_child_call();

  ItemCache *try_get_storage_proxy(AddressAndKey const &query);

  void push_new_storage_proxy(AddressAndKey const &new_addrkey,
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

  std::map<AddressAndKey, ItemCache> const &get_storage_proxies() const
  {
    return storage_proxies_;
  }

  // TODO(geoff) set based on metadata
  const bool unwind_catch_boundary_ = false;
  const Invocation active_invocation_;
  AuthManager auth_manager_;
  const uint64_t flags_;

private:
  ReturnBuffer to_return_;

  // Cached here separately from the primary SystemCallHandler
  // in order to (in later PR) support subcall unwinding
  // swap to non unique_ptr
  std::map<AddressAndKey, ItemCache> storage_proxies_;

  std::vector<LogEntry> logs_;
};

} // namespace hogd
