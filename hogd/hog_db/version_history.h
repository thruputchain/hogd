#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace hogd {

// Most touched keys have one version. Only nested savepoints allocate a
// backing vector; the oldest version remains in the map node throughout.
template<typename T> class VersionHistory {
public:
  VersionHistory() = default;
  VersionHistory(VersionHistory &&other) noexcept
    : first_(std::move(other.first_)), rest_(std::move(other.rest_))
  {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    other.first_.reset();
  }

  bool empty() const { return !first_; }
  size_t size() const { return first_.has_value() + rest_.size(); }
  T &back() { return rest_.empty() ? *first_ : rest_.back(); }
  const T &back() const { return rest_.empty() ? *first_ : rest_.back(); }
  const T &operator[](size_t n) const { return n ? rest_[n - 1] : *first_; }

  template<typename... Args> T &emplace_back(Args &&...args)
  {
    if (!first_)
      return first_.emplace(std::forward<Args>(args)...);
    return rest_.emplace_back(std::forward<Args>(args)...);
  }
  void push_back(T value) { emplace_back(std::move(value)); }

  void discard_after(unsigned depth)
  {
    while (!rest_.empty() && rest_.back().tx_depth > depth)
      rest_.pop_back();
    if (rest_.empty() && first_ && first_->tx_depth > depth)
      first_.reset();
  }

private:
  std::optional<T> first_;
  std::vector<T> rest_;
};

} // namespace hogd
