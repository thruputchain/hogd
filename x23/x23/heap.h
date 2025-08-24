// -*- C++ -*-

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <ranges>
#include <utility>
#include <vector>

namespace x23 {

using std::ptrdiff_t;
using std::size_t;

// A heap that gives you a HeapIndex handle you can use to remove
// arbitrary elements from the heap.  The HeapIndex is updated as
// things shift around the heap, allowing the Heap itself to be a big
// array.
template<typename T, typename Cmp = std::ranges::less, size_t Degree = 4>
requires (Degree >= 2)
class Heap {
  using value_type = T;

public:
  class HeapIndex {
  public:
    HeapIndex() noexcept = default;

    // non-moveable, because the Heap maintains a back pointer to the
    // HeapIndex to update it.
    HeapIndex(HeapIndex &&) noexcept = delete;

    // True if it corresponds to a valid index that can be removed.
    explicit operator bool() const { return idx_ >= 0; }

    ptrdiff_t get() const { return idx_; } // mostly for debugging

  private:
    ptrdiff_t idx_ = -1;
    friend Heap;
  };

  Heap() noexcept = default;
  Heap(Heap &&) noexcept = default;
  Heap &operator=(Heap &&) noexcept = default;

  // Insert a new element in the Heap.  If ip is non-NULL, it will be
  // filled in with a HeapIndex that can be used to fetch or remove
  // the item just inserted.
  template<typename U> void insert(U &&u, HeapIndex *ip = nullptr)
  {
    size_t i = elems_.size();
    elems_.emplace_back(std::forward<U>(u), ip);
    if (ip)
      ip->idx_ = i;
    sift_up(i, false);
  }

  size_t size() const noexcept { return elems_.size(); }
  bool empty() const { return elems_.empty(); }

  // Return a reference to the least element in the Heap.
  decltype(auto) top(this auto &&self) { return self.elems_.front().t_; }

  // Remove the least element from the Heap.
  void pop()
  {
    assert(!empty());
    HeapIndex *ip = elems_.front().ip_;
    if (elems_.size() > 1) {
      swap(elems_.front(), elems_.back());
      elems_.pop_back();
      sift_down(0);
    }
    else
      elems_.pop_back();
    if (ip)
      ip->idx_ = -1;
  }

  // Remove and return the least element in the Heap.
  T poptop()
  {
    T t{std::move(top())};
    pop();
    return t;
  }

  // Remove an arbitrary element of the Heap given its HeapIndex
  void remove(const HeapIndex &ip)
  {
    assert(valid_index(ip));
    remove_index(ip.idx_);
  }

  // Returns true if the HeapIndex is valid and points to an element
  // in this heap.
  bool valid_index(const HeapIndex &ip) const
  {
    return ip.idx_ >= 0 && ip.idx_ < elems_.size() &&
           &ip == elems_[ip.idx_].ip_;
  }

  // Methods below here mostly for debugging/printing heaps

  // Access an element in the Heap given its HeapIndex
  decltype(auto) operator[](this auto &&self, const HeapIndex &ip)
  {
    assert(self.valid_index(ip));
    return self.index(ip.idx_);
  }

  auto view(this auto &&self)
  {
    return self.elems_ | std::views::transform(&Element::t_);
  }

  // Access element at numeric index
  decltype(auto) index(this auto &&self, size_t i) { return self.elems_[i].t_; }

  // Remove an element by numeric index
  void remove_index(size_t i)
  {
    sift_up(i, true);
    pop();
  }

  void check() noexcept
  {
#ifndef NDEBUG
    for (size_t i = 1; i < elems_.size(); ++i) {
      assert(!(elems_[i] < elems_[parent(i)]));
      if (elems_[i].ip_)
        assert(elems_[i].ip_->idx_ == ptrdiff_t(i));
    }
#endif // !NDEBUG
  }

private:
  struct Element {
    T t_;
    HeapIndex *ip_ = nullptr;

    friend bool operator<(const Element &a, const Element &b)
    {
      return Cmp{}(a.t_, b.t_);
    }
    friend void swap(Element &a, Element &b)
    {
      using std::swap;
      swap(a.t_, b.t_);
      swap(a.ip_, b.ip_);
      ptrdiff_t distance = &b - &a;
      if (a.ip_)
        a.ip_->idx_ -= distance;
      if (b.ip_)
        b.ip_->idx_ += distance;
    }
  };

  std::vector<Element> elems_;

  static constexpr size_t kDegree = Degree;
  static constexpr size_t parent(size_t i) { return (i - 1) / kDegree; }
  static constexpr size_t first_child(size_t i) { return kDegree * i + 1; }

  // One more than the index of the last child of node i
  size_t end_child(size_t i)
  {
    return std::min(first_child(i) + kDegree, elems_.size());
  }

  // Assuming the min-heap property applies to all elements except i,
  // keep moving the element that started at index i up in the tree
  // until it becomes the root or, if all_the_way is false, until it
  // is not smaller than its parent.  If all_the_way is false, this
  // function is also sometimes called "decrease-key."  If all_the_way
  // is true, this function behaves like decrease-key where node i's
  // key is negative infinity.
  void sift_up(size_t i, bool all_the_way)
  {
    while (i != 0) {
      size_t up = parent(i);
      if (!all_the_way && !(elems_[i] < elems_[up]))
        return;
      swap(elems_[i], elems_[up]);
      i = up;
    }
  }

  // Assuming each child of node i is already the root of a subtree
  // that is a min-heap, move i down if it is not less than or equal
  // to all of its children.  This function is also commonly known as
  // "heapify."
  void sift_down(size_t i)
  {
    for (;;) {
      size_t best = i;
      for (size_t c = first_child(i), e = end_child(i); c < e; ++c)
        if (elems_[c] < elems_[best])
          best = c;
      if (i == best)
        return;
      swap(elems_[i], elems_[best]);
      i = best;
    }
  }
};

} // namespace x23
