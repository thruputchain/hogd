#include "hogd/hogexec/runtime/snapshot_storage_item.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

SystemStatus<const RetrocausalQueueBlock *>
SnapshotStorageItem::get_next_rcq_block(uint64_t search_start_block)
{
  // Only valid for RCQ items
  if (!std::holds_alternative<RetrocausalQueueEntry>(entry_)) {
    return nullptr;
  }
  auto &rcq = std::get<RetrocausalQueueEntry>(entry_);
  std::lock_guard<std::mutex> lock(rcq_mtx_);
  return rcq.get_next_block(db_proxy_, block_id_, addrkey_, search_start_block);
}

SystemStatus<Nondeterministic<std::optional<QueueEntry>>>
SnapshotStorageItem::get_concurrent_rcq_entry(uint32_t index)
{
  std::lock_guard<std::mutex> lock(rcq_mtx_);
  if (index < concurrent_queue_entries_.size()) {
    return concurrent_queue_entries_[index];
  }

  auto db_res =
      db_proxy_.get_concurrent_retrocausal_queue_entry(block_id_, addrkey_,
                                                       index)();
  if (!db_res) {
    return db_res.error();
  }
  auto entry = db_res.value();
  if (entry.has_value()) {
    concurrent_queue_entries_.push_back(*entry);
  }
  return entry;
}

} // namespace hogd
