// -*-C++-*-
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace x23::detail {

// XIDs are opaque to the server. Choose one whose low bits address an
// unused slot, and retain the whole XID to reject stale/unknown replies.
// The caller provides synchronization and removes a callback before
// invoking it, since a completed call can immediately issue another call.
template<typename T> class XidMap {
  struct Slot {
    std::uint32_t xid{};
    T cb{};
  };
  std::vector<Slot> slots_;
  size_t size_{};
  std::uint32_t lastxid_{};

  void grow()
  {
    std::vector<Slot> fresh(std::max(size_t{16}, slots_.size() * 2));
    const size_t mask = fresh.size() - 1;
    // Doubling the mask cannot make two previously distinct slots collide.
    for (auto &s : slots_)
      if (s.cb)
        fresh[s.xid & mask] = std::move(s);
    slots_ = std::move(fresh);
  }

public:
  XidMap() = default;
  explicit XidMap(std::uint32_t lastxid) : lastxid_(lastxid) {}

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return slots_.size(); }

  template<typename U> std::uint32_t insert(U &&cb)
  {
    // Keep at least half the slots free to bound the search for an XID.
    // Grow before constructing a CoroCB, which launches its Awaitable.
    if (size_ >= slots_.size() / 2)
      grow();
    const size_t mask = slots_.size() - 1;
    for (;;) {
      const auto xid = ++lastxid_;
      auto &slot = slots_[xid & mask];
      if (slot.cb)
        continue;
      slot.xid = xid;
      slot.cb = T(std::forward<U>(cb));
      ++size_;
      return xid;
    }
  }

  T take(std::uint32_t xid)
  {
    if (slots_.empty())
      return {};
    auto &slot = slots_[xid & (slots_.size() - 1)];
    if (slot.xid != xid || !slot.cb)
      return {};
    --size_;
    return std::exchange(slot.cb, {});
  }

  template<typename F> void for_each(F &&f)
  {
    for (auto &slot : slots_)
      if (slot.cb)
        f(slot.cb);
  }
};

} // namespace x23::detail
