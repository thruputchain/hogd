#include "hogd/ramdb/item_allocator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/loglet.h"
#include "hogd/ramdb/scalable_map.h"

namespace hogd {

namespace {

// The per-thread relocation destinations, used by compaction on the WorkerPool.
//
// Safe there because Database destroys pool_ before loglet_allocator_, so a
// worker's destinations are released while the loglet allocator is still alive.
// It is *not* safe for a pass on an arbitrary caller thread: those thread_locals
// are destroyed at thread exit, potentially long after the allocator, and
// freeing a loglet through a dangling allocator aborts inside its mutex. That
// was BUG-3, and it is why consolidation owns its destinations in the
// ItemAllocator instead of borrowing this.
detail::CompactDests &
compact_dests()
{
  static thread_local detail::CompactDests dests;
  return dests;
}

} // namespace

namespace detail {

ItemHead::~ItemHead()
{
  try {
    if (head_)
      allocator_->give_to_compactor(std::move(head_));
  } catch (const std::exception &e) {
    // must panic because if give_to_compactor throws, head_ will be freed.
    panic("Couldn't free ItemHead: {}", e.what());
  }
}

inline Loglet *
ItemHead::get(ItemAllocator &allocator, size_t kv_len)
{
  assert(!head_ || allocator_ == &allocator);

  // Try to use the current head.
  if (head_ && head_->bytes_left() >= sizeof(ItemHeader) + kv_len) [[likely]]
    return head_.get();

  // Otherwise the head is full or has not yet been allocated. Allocate
  // a new one. head_ can be nullptr after this if the compactor is unable
  // to find free space.
  roll_over(allocator);

  return head_.get();
}

inline void
ItemHead::roll_over(ItemAllocator &allocator)
{
  allocator_ = &allocator;

  if (head_)
    allocator_->give_to_compactor(std::move(head_));
  head_ = make_unique_loglet(allocator_->allocator_, LogletMode::kMemoryItems);
}

inline void
ItemHead::release(ItemAllocator &allocator)
{
  if (!head_)
    return;
  allocator_ = &allocator;
  allocator_->give_to_compactor(std::move(head_));
}

} // namespace detail

ItemHeader *
ItemAllocator::alloc(uint8_t type, size_t key_len, size_t value_len)
{
  Loglet *ll = head_->get(*this, key_len + value_len);
  if (!ll) [[unlikely]]
    return nullptr;

  ItemHeader *hdr = ll->alloc(ItemStorage::kNormal, type, key_len, value_len);
  assert(hdr != nullptr); // cannot fail; we checked the space left.
  return hdr;
}

size_t
ItemAllocator::compact(float utilization_thresh)
{
  // move_if moves the elements for which pred is true to a new vector and
  // returns it. The src vector is modified to contain the remaining elements
  // where pred is false.
  auto move_if = []<typename T, typename Func>(std::vector<T> &src, Func pred) {
    auto tail = std::ranges::partition(src, std::not_fn(pred));
    std::vector<T> ret(std::make_move_iterator(tail.begin()),
                       std::make_move_iterator(tail.end()));
    src.erase(tail.begin(), tail.end());
    return ret;
  };

  // record start time for measuring compaction time.
  ElapsedTimer timer;

  // take all the compactable loglets.
  std::vector<UniqueLoglet> claimed;
  {
    std::scoped_lock guard(lock_);
    claimed = move_if(compactable_loglets_, [utilization_thresh](
                                                const UniqueLoglet &ll) {
      return !ll->fully_compacted() && ll->utilization() <= utilization_thresh;
    });
  }

  // Hand each claimed loglet to a worker. We hold our own shared_ptr in
  // `in_flight` so that if pool_.submit() throws bad_alloc (inside
  // WorkerPool's promise / UniqueFunction / std::queue allocations) after
  // its inner lambda is constructed, the loglet survives the lambda's
  // destruction and we can put it back into compactable_loglets_.
  std::vector<std::shared_ptr<UniqueLoglet>> in_flight;
  std::vector<std::future<CompactionStats>> completions;
  CompactionStats stats{};

  try {
    in_flight.reserve(claimed.size());
    completions.reserve(claimed.size());

    for (auto &ll : claimed) {
      in_flight.push_back(std::make_shared<UniqueLoglet>(std::move(ll)));
      completions.emplace_back(pool_.submit(
          [this, l = in_flight.back()] { return compact_one(std::move(*l)); }));
    }

    // wait for the compactions to finish.
    for (std::future<CompactionStats> &f : completions)
      stats += f.get();
  } catch (const std::bad_alloc &) {
    // Drain any submitted workers so their std::move(*l) settles before
    // we inspect in_flight. compactable_loglets_'s emplace_back below
    // cannot throw because the ItemAllocator constructor reserved capacity
    // to LogletAllocator::loglet_count() — the maximum possible occupancy.
    for (std::future<CompactionStats> &f : completions) {
      try {
        // We call get() only to drain the worker and let its std::move(*l)
        // settle before we inspect in_flight; the [[nodiscard]] stats are
        // intentionally unused here.
        [[maybe_unused]] auto stats = f.get();
      } catch (...) { // NOLINT(bugprone-empty-catch)
        // Drain only — a worker exception here is suppressed because we are
        // already unwinding compact() on bad_alloc and have no useful path
        // to surface it. compact_one is allocation-free post the ctor's
        // reserve, so this catch is not expected to fire in practice.
      }
    }
    std::scoped_lock guard(lock_);
    for (std::shared_ptr<UniqueLoglet> &l : in_flight)
      if (l && *l)
        compactable_loglets_.emplace_back(std::move(*l));
    for (UniqueLoglet &ll : claimed)
      if (ll)
        compactable_loglets_.emplace_back(std::move(ll));
    warn("compact: deferred {} loglets after bad_alloc", claimed.size());
    return 0;
  }

  // print the compaction rate.
  auto seconds = timer.elapsed();
  info("finished compaction in {:.3f} seconds", seconds);
  if (stats.items > 0) {
    // A fast pass can measure zero elapsed time even on a steady clock; guard
    // the division so the rate is never non-finite.
    double items_per_sec =
        seconds > 0.0 ? static_cast<double>(stats.items) / seconds : 0.0;
    info("scanned {:.0f} items/sec ({}% live)", items_per_sec,
         stats.live_items * 100 / stats.items);
  }
  else {
    warn("no items scanned.");
  }

  // determine how much memory was freed.
  // A pass that took more loglets than it released reclaimed nothing; report
  // it as such rather than letting the negative net wrap into a huge figure
  // that would convince background_worker memory had been recovered.
  size_t free_bytes =
      stats.loglets_freed > 0
          ? static_cast<size_t>(stats.loglets_freed) * kLogletRegionSize
          : 0;
  size_t total_bytes = allocator_.size();
  info("reclaimed {}/{} ({}% freed)", print_iec_bytes(free_bytes),
       print_iec_bytes(total_bytes), free_bytes * 100 / total_bytes);
  allocator_.print_stats();
  return free_bytes;
}

void
ItemAllocator::flush_consolidation_dests()
{
  if (consolidate_dests_.cold)
    give_to_compactor(std::move(consolidate_dests_.cold));
  if (consolidate_dests_.hot)
    give_to_compactor(std::move(consolidate_dests_.hot));
}

void
ItemAllocator::roll_over_heads()
{
  std::unique_lock ul = head_.lock_threads();
  head_.for_each(ul, [this](detail::ItemHead &h) { h.release(*this); });
}

size_t
ItemAllocator::consolidate(const std::function<bool(ItemHeader *)> &is_hot,
                           float min_mixedness, size_t max_victims)
{
  ElapsedTimer timer;

  // Examine each loglet that was compactable when the pass started, once. The
  // budget is fixed up front because non-victims go back on the end of the
  // list: without it a pass would keep meeting the loglets it just returned.
  size_t budget = 0;
  {
    std::scoped_lock guard(lock_);
    budget = compactable_loglets_.size();
  }

  // Claim candidates a batch at a time rather than swapping the whole list out.
  // Measuring hotness walks every item header, which is far too slow to do
  // under lock_, but claiming everything leaves compactable_loglets_ empty for
  // the duration -- a concurrent compact() would then find nothing, reclaim
  // nothing, and background_worker would escalate. A batch bounds how much is
  // invisible at once.
  static constexpr size_t kClaimBatch = 32;

  CompactionStats stats{};
  size_t count = 0;
  size_t examined = 0;
  std::vector<UniqueLoglet> batch;
  batch.reserve(kClaimBatch);

  while (examined < budget && (max_victims == 0 || count < max_victims)) {
    batch.clear();
    {
      std::scoped_lock guard(lock_);
      size_t n = std::min(kClaimBatch, compactable_loglets_.size());
      auto first = compactable_loglets_.begin();
      auto last = first + static_cast<ptrdiff_t>(n);
      batch.insert(batch.end(), std::make_move_iterator(first),
                   std::make_move_iterator(last));
      compactable_loglets_.erase(first, last);
    }
    if (batch.empty())
      break;
    examined += batch.size();

    // Select by mixedness. This is the opposite rule from compact():
    // fragmentation is irrelevant here, and a fully compacted loglet is a
    // perfectly good victim.
    //
    // Relocation runs on this thread rather than the pool: consolidation is a
    // background activity with no memory-pressure deadline behind it, and
    // serial relocation keeps the two per-thread destinations from multiplying
    // across workers and draining the compaction reserve.
    for (UniqueLoglet &ll : batch) {
      if (!ll)
        continue;
      // A loglet with nothing live in it has no layout to improve, and at
      // min_mixedness 0 it would otherwise be selected and counted. Rolling the
      // write heads over produces one of these per idle thread, which is enough
      // to dominate the victim count. Reclaiming them is compact()'s job.
      if (ll->live_size() == 0) {
        give_to_compactor(std::move(ll));
        continue;
      }
      // Check the budget before measuring rather than after. measure_hotness()
      // walks every item header in the loglet, so it is the dominant cost of
      // examining a candidate -- spending it on loglets this pass has already
      // decided it cannot relocate turns a bounded pass into a full scan. At a
      // 32-loglet batch that was most of the cost of a one-victim pass.
      if (max_victims != 0 && count >= max_victims) {
        give_to_compactor(std::move(ll));
        continue;
      }
      ll->measure_hotness(is_hot);
      if (ll->mixedness() >= min_mixedness) {
        stats += compact_one(std::move(ll), &is_hot, &consolidate_dests_);
        ++count;
      }
      else {
        give_to_compactor(std::move(ll));
      }
    }
  }

  if (count == 0) {
    info("consolidate: no loglets above mixedness {:.2f}", min_mixedness);
    return 0;
  }

  // Deliberately does *not* flush consolidate_dests_. A partly filled hot
  // destination is what the next pass keeps packing into, and flushing it here
  // is what made a sequence of small passes useless: each pass would start a
  // fresh, nearly empty hot loglet, so 20 passes of 2 loglets spread the hot
  // items over 20 sparse loglets where one pass of 40 packed them into a few.
  // Measured: 40 loglets relocated that way reached hit 0.7624, against 0.9047
  // for 20 in a single pass. See flush_consolidation_dests() for ending a run.
  auto seconds = timer.elapsed();
  info("consolidated {} loglets in {:.3f} seconds ({} items, {} live)", count,
       seconds, stats.items, stats.live_items);
  return count;
}

detail::CompactionStats
ItemAllocator::compact_one(UniqueLoglet &&src,
                           const std::function<bool(ItemHeader *)> *is_hot,
                           detail::CompactDests *dests)
{
  if (dests == nullptr)
    dests = std::addressof(compact_dests());

  CompactionStats stats{};
  size_t bytes_moved = 0;
  // set when the reserved pool runs dry mid-loglet, which leaves live items
  // behind in src and makes it unsafe to free.
  bool exhausted = false;

  // alloc_loglet is a utility for allocating reserved loglets. It returns an
  // empty UniqueLoglet if the reserve is exhausted; every caller must check.
  auto alloc_loglet = [this, &stats]() {
    UniqueLoglet dst =
        make_unique_loglet(allocator_, LogletMode::kMemoryItems, true);
    if (dst) [[likely]]
      stats.loglets_freed--;
    return dst;
  };

  // compact items in this loglet: move live items and skip dead items.
  for (ItemHeader &h : *src) {
    // stop as soon as there is nowhere left to relocate to.
    if (exhausted) [[unlikely]]
      break;

    stats.items++;
    ItemHeader *src_item = std::addressof(h);

    // loglet compaction is finished if every live item has been moved.
    if (src->live_size() == bytes_moved)
      break;

    // optimization: skip dead items that can be detected without locking. The
    // atomic_ref must not outlive this check — Loglet::clone(src_item) below
    // performs a plain copy of src_item->storage, and [atomics.ref.generic]/3
    // forbids ordinary access to an object while an atomic_ref to it exists.
    if (std::atomic_ref<ItemStorage>(src_item->storage)
            .load(std::memory_order_acquire) == ItemStorage::kTombstone)
      continue;

    // try to move the item to the new loglet.
    [[maybe_unused]] bool found = map_->modify_locked(
        src_item->get_key(),
        [this, alloc_loglet, src_item, is_hot, dests, &bytes_moved, &stats,
         &exhausted](ItemHeader **hptr, size_t /* hash */) mutable {
          // modify_locked matched by key, not by pointer. A concurrent
          // different-sized put or remove+put may have already unlinked
          // src_item and inserted a new same-key header; unlink()
          // tombstones src_item under this same bucket lock. *hptr is
          // then the new live header, and cloning src_item over it would
          // clobber the live value with stale bytes. src_item is already
          // dead, so skip it; its memory is reclaimed when this source
          // loglet is freed at the end of the pass.
          if (*hptr != src_item)
            return;

          // Pick the destination by temperature. Each carries over between
          // calls and is handed to the compactor when it fills, exactly as the
          // single destination always did.
          UniqueLoglet &dst = (is_hot != nullptr && (*is_hot)(src_item))
                                  ? dests->hot
                                  : dests->cold;

          // initialize the first use of the per-thread loglet. Leaving the
          // item where it is costs only a deferred relocation; dereferencing
          // an exhausted reserve would be a crash.
          if (!dst) [[unlikely]] {
            dst = alloc_loglet();
            if (!dst) [[unlikely]] {
              exhausted = true;
              return;
            }
          }

          // allocate the destination item
          ItemHeader *dst_item = dst->clone(src_item);
          if (!dst_item) {
            // loglet is full; get a new one.
            give_to_compactor(std::move(dst));
            dst = alloc_loglet();
            if (!dst) [[unlikely]] {
              exhausted = true;
              return;
            }
            dst_item = dst->clone(src_item);
            assert(dst_item != nullptr);
          }

          // copy the item's key and value to the new location.
          std::ranges::copy(src_item->get_key_and_value(),
                            dst_item->get_key_and_value().begin());

          // point the map to the new location (bucket lock is held).
          dst_item->next = src_item->next;
          *hptr = dst_item;
          stats.live_items++;
          bytes_moved += dst_item->total_bytes();
        });
  }

  if (exhausted) [[unlikely]] {
    // Live items are still in src, so freeing it would lose them. Put it back
    // on the compactable list for a later pass instead. The pass reclaims less
    // than it hoped to, which background_worker already handles: a compaction
    // that frees nothing escalates, and a still-starved allocator reports
    // kNoMemory rather than crashing.
    warn("compaction reserve exhausted; deferring a partially compacted "
         "loglet");
    give_to_compactor(std::move(src));
    return stats;
  }

  stats.loglets_freed++;
  return stats;
}

} // namespace hogd
