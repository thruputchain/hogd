// -*- C++ -*-

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
// things shift around the heap, so that the Heap itself can just be a
// big array.
template<typename T, typename Cmp = std::less<T>> class Heap {
public:
  class HeapIndex {
  public:
    HeapIndex() = default;

    // non-moveable, because the Heap maintains a back pointer to the
    // HeapIndex to update it.
    HeapIndex(HeapIndex &&) = delete;

    // True if it corresponds to a valid index that can be removed.
    explicit operator bool() const { return idx_ >= 0; }

    ptrdiff_t get() const { return idx_; } // mostly for debugging

  private:
    ptrdiff_t idx_ = -1;
    friend Heap;
  };

  // Insert a new element in the Heap.  If ip is non-NULL, it will be
  // filled in with a HeapIndex that can be used to fetch or remove
  // the item just inserted.
  template<typename U> void emplace(U &&u, HeapIndex *ip = nullptr)
  {
    int i = elems_.size();
    elems_.emplace_back(Element{std::forward<U>(u), ip});
    if (ip)
      ip->idx_ = i;
    sift_up(i, false);
  }

  bool empty() const { return elems_.empty(); }

  // True if there are items in the Heap.
  explicit operator bool() const { return !empty(); }

  // Return a reference to the least element in the Heap.
  T &top() { return elems_.front().t_; }

  // Remove the least element from the Heap.
  void pop()
  {
    HeapIndex *ip = elems_.front().ip_;
    if (elems_.size() > 1) {
      swap(elems_.front(), elems_.back());
      elems_.resize(elems_.size() - 1);
      sift_down(0);
    }
    else
      elems_.resize(0);
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
  void remove(const HeapIndex &ip) { remove_index(ip.idx_); }

  // Access an element in the Heap given its HeapIndex
  T &operator[](const HeapIndex &ip) { return index(ip.idx_); }
  const T &operator[](const HeapIndex &ip) const { return index(ip.idx_); }

  // Returns true if the HeapIndex is valid and points to an element
  // in this heap.
  bool valid_index(const HeapIndex &ip) const
  {
    return ip->idx_ >= 0 && ip->idx_ < elems_.size() &&
           ip == elems_[ip->idx_].ip_;
  }

  // Methods below here mostly for debugging/printing heaps

  // Remove an element by numeric index
  void remove_index(size_t i)
  {
    assert(i < elems_.size());
    sift_up(i, true);
    pop();
  }

  auto view() { return elems_ | std::views::transform(&Element::t_); }
  auto view() const { return elems_ | std::views::transform(&Element::t_); }

  size_t size() const { return elems_.size(); }
  T &index(size_t i) { return elems_.at(i).t_; }
  const T &index(size_t i) const { return elems_.at(i).t_; }

  void check()
  {
#ifndef NDEBUG
    for (size_t i = 1, e = elems_.size(); i < e; ++i) {
      assert(!(elems_[i] < elems_[parent(i)]));
      if (elems_[i].ip_)
        assert(elems_[i].ip_->idx_ == i);
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

  static constexpr size_t kDegree = 2;
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
  // is true, this function is like decrease-key as if you decreased
  // the key to negative infinity.
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
