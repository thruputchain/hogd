#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <vector>

#include "transaction.hh"

namespace hogd {

class CallFrame {
public:
  CallFrame(Invocation invocation);

  void commit_successful_child_call(CallFrame &frame);
  // revert the child call frame
  void commit_failed_child_call();

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

  const Invocation active_invocation_;

  std::map<ContractAddress, SHA256Hash> new_contract_deployments_;

private:
  ReturnBuffer to_return_;

  std::vector<LogEntry> logs_;
};

} // namespace hogd
