// -*-C++-*-

// Vec is a vector class similar to std::vector and std::string.  It
// provides a few features that are prohibited by abstraction on the
// std:: classes.  Specifically:
//
//   - The resize_uninitialized method allows you to resize a vector
//     or string without initializing the new memory (assuming the
//     element type is trivially constructed).  Yes this is dangerous,
//     but if you are about to marshal some data or read in a file,
//     there's no need to zero the memory first.
//
//   - There's a parameter to enforce a maximum size.  This is useful
//     for data structures that are to be marshaled/unmarshaled, such
//     as data structures specified in the XDR interface definition
//     language, which allows bounded strings and vectors.  Without
//     this limitation, you still need to check things against
//     PTRDIFF_MAX/sizeof(T), so allowing a lower limit doesn't really
//     cost much.
//
//   - The string type Str is implemented in terms of the vector Vec,
//     simplifying code and ensuring the two will work consistently.
//     Thanks to C++23 deducing this (P0847R6), methods like operator+
//     can return the correct type, saving Str from reimplementing a
//     lot of things.
//
//   - You can convert between vectors of signed and unsigned bytes by
//     means of the destructive Uvec::reinterpret method, which is
//     kind of like reinterpret_cast for vector.  For example:
//
//        Str x = "Hello world\n"_ustr;
//        auto y = auto(x).reinterpret<Vec<unsigned char>>();
//        assert(!y.empty());
//        auto z = std::move(y).reinterpret<Str>();
//        assert(y.empty());  // wiped by reinterpret
//        assert(x == z);

#pragma once

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <type_traits>

#include <x23/cleanup.h>
#include <x23/config.h>

namespace x23 {

using std::size_t;

// Any error that could be related to bad XDR input during
// marshalling, as opposed to straight-up logic errors in the code.
struct XdrException : std::exception {
  std::string what_;

  XdrException(std::string what) : what_(std::move(what)) {}

  template<typename... Args> requires (sizeof...(Args) > 0)
  XdrException(std::format_string<Args...> fmt, Args &&...args)
    : XdrException(std::vformat(fmt.get(), std::make_format_args(args...)))
  {}

  const char *what() const noexcept override { return what_.c_str(); }
};

struct XdrLengthError : XdrException {
  using XdrException::XdrException;
};

namespace detail {

// We'd like to use the same allocator for types with the same size
// and alignment requirements, so that, for instance, we can
// reinterpret between Vecs of char and std::uint8_t.  Otherwise, it
// would be incorrect to allocate memory with std::allocator<char> and
// then free it with std::allocator<std::uint8_t>.
// merged_allocator<T>::typename type will therefore collapse similar
// types T to the same allocator.
//
// Complicating matters, we want to be able to have recursive
// vectors--e.g., a struct S that contains a Vec<S>.  In that case we
// won't know the size or alignment of S at the time we are resolving
// the template arguments, so can't really merge the allocators.
// However, we probably shouldn't be reinterpreting between vectors of
// structures anyway, so just use std::allocator<T> for those.
//
// As a final point, you might wonder why bother with std::allocator
// altogether instead of just directly calling ::operator new(size_t).
// The reason is that ::operator new is not constexpr, while
// std::allocator<T>::allocate is.  Of course, since as of this
// writing gcc currently has slightly broken if consteval support, the
// constexpr stuff will need to evolve anyway.
template<typename T> struct merged_allocator {
  using type = std::allocator<T>;
};
// C.f. deprecated std::aligned_storage
template<size_t Size, size_t Alignment>
struct alignas(Alignment) AlignedStorage {
  char data_[Size];
};
template<typename T> requires std::is_scalar_v<T>
struct merged_allocator<T> {
  using type = std::allocator<AlignedStorage<sizeof(T), alignof(T)>>;
};

// Like std::strlen, but constexpr
constexpr size_t
strlen(const char *s)
{
  if consteval {
    size_t ret{};
    while (*s++)
      ++ret;
    return ret;
  }
  else {
    return std::strlen(s);
  }
}

template<typename T> class PtrIterator {
public:
  using iterator_category = std::contiguous_iterator_tag;
  using difference_type = std::ptrdiff_t;
  using value_type = T;
  using pointer = T *;
  using reference = T &;

  constexpr PtrIterator() noexcept : ptr_(nullptr) {}
  constexpr PtrIterator(pointer ptr) noexcept : ptr_(ptr) {}

  template<typename U>
  requires std::constructible_from<U *, T *> &&
           std::same_as<std::remove_cv_t<U>, std::remove_cv_t<T>>
  constexpr PtrIterator(const PtrIterator<U> &other) noexcept : ptr_(other.ptr_)
  {}

  constexpr decltype(auto) operator*(this const auto &self) noexcept
  {
    return *self.ptr_;
  }
  constexpr auto operator->(this const auto &self) noexcept
  {
    return self.ptr_;
  }

  constexpr auto &operator++(this auto &self) noexcept
  {
    ++self.ptr_;
    return (self);
  }
  constexpr auto operator++(this auto &self, int) noexcept
  {
    auto tmp = self;
    ++self;
    return tmp;
  }

  constexpr auto &operator--(this auto &self) noexcept
  {
    --self.ptr_;
    return (self);
  }
  constexpr auto operator--(this auto &self, int) noexcept
  {
    auto tmp = self;
    --self;
    return tmp;
  }

  constexpr auto &operator+=(this auto &self, difference_type offset) noexcept
  {
    self.ptr_ += offset;
    return (self);
  }
  template<typename Self>
  constexpr Self operator+(this const Self &self,
                           difference_type offset) noexcept
  {
    return Self(self.ptr_ + offset);
  }
  template<std::derived_from<PtrIterator> Self>
  friend constexpr Self operator+(difference_type offset,
                                  const Self &self) noexcept
  {
    return Self(self.ptr_ + offset);
  }

  constexpr auto &operator-=(this auto &self, difference_type offset) noexcept
  {
    self.ptr_ -= offset;
    return (self);
  }
  template<typename Self>
  constexpr Self operator-(this const Self &self,
                           difference_type offset) noexcept
  {
    return Self(self.ptr_ - offset);
  }
  template<typename Self>
  constexpr difference_type operator-(this const Self &self,
                                      const Self &other) noexcept
  {
    return self.ptr_ - other.ptr_;
  }

  constexpr decltype(auto) operator[](this const auto &self,
                                      difference_type offset) noexcept
  {
    return self.ptr_[offset];
  }

  template<typename Self>
  constexpr auto operator<=>(this const Self &self, const Self &other) noexcept
  {
    return self.ptr_ <=> other.ptr_;
  }

  template<typename Self>
  constexpr bool operator==(this const Self &self, const Self &other) noexcept
  {
    return self.ptr_ == other.ptr_;
  }

private:
  pointer ptr_;
};

} // namespace detail

template<typename T> constexpr size_t kMaxArraySize = PTRDIFF_MAX / sizeof(T);

template<typename T, size_t MaxSize = kMaxArraySize<T>,
         typename Allocator = detail::merged_allocator<T>::type>
class Vec {
public:
  using value_type = T;
  using size_type = size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T &;
  using const_reference = const T &;
  using pointer = T *;
  using const_pointer = const T *;
  struct iterator : detail::PtrIterator<value_type> {
    using detail::PtrIterator<value_type>::PtrIterator;
  };
  struct const_iterator : detail::PtrIterator<const value_type> {
    using detail::PtrIterator<const value_type>::PtrIterator;
  };
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using allocator_type = Allocator;

  struct Buf {
    T *begin;
    T *end;
    T *bufend;
  };

  static constexpr size_type kMaxSize = MaxSize;

protected:
  [[no_unique_address]] allocator_type a_{};
  Buf b_{};

public:
  constexpr Vec(const allocator_type &a = {}) noexcept : a_(a) {}
  constexpr Vec(Vec &&other) noexcept : a_(std::move(other.a_)), b_(other.b_)
  {
    other.b_ = {};
  }
  constexpr Vec(const Vec &other) : Vec(std::from_range, other) {}

  template<size_t MS>
  constexpr Vec(Vec<value_type, MS, allocator_type> &&other)
      noexcept(max_size() >= Vec<value_type, MS, allocator_type>::max_size())
    : b_(other.b_)
  {
    if constexpr (other.max_size() > max_size())
      // Okay to throw, other will still delete memory
      check_size(other.size());
    a_ = std::move(other.a_);
    other.b_ = {};
  }
  template<size_t MS>
  constexpr Vec(const Vec<value_type, MS, allocator_type> &other)
    : Vec(std::from_range, other)
  {}

  constexpr explicit Vec(size_type count, const value_type &v = {})
  {
    resize(count, v);
  }

  template<std::random_access_iterator IterIn>
  constexpr Vec(IterIn first, IterIn last)
    : Vec(std::from_range, std::ranges::subrange(first, last))
  {}
  constexpr Vec(std::initializer_list<value_type> init) { append_range(init); }
  template<std::ranges::range R> constexpr Vec(std::from_range_t, R &&r)
  {
    try {
      append_range(std::forward<R>(r));
    } catch (...) {
      resize(0);
      throw;
    }
  }

  // Note we deduce the type of a because it really has to match
  // allocator_type, otherwise since std::allocator<T> can be
  // constructed from std::allocator<U> when sizeof(T) != sizeof(U),
  // we won't properly free the memory in b.
  explicit Vec(const Buf &b,
               const std::same_as<allocator_type> auto &a) noexcept
    : a_(a), b_(b)
  {}

  constexpr ~Vec() { realloc(0); }

  constexpr Vec &operator=(Vec &&other) noexcept
  {
    realloc(0);
    a_ = std::move(other.a_);
    b_ = other.b_;
    other.b_ = {};
    return *this;
  }
  constexpr Vec &operator=(const Vec &other)
  {
    do_copy(other);
    return *this;
  }

  template<size_t MS>
  constexpr Vec &operator=(Vec<T, MS, allocator_type> &&other)
      noexcept(max_size() <= Vec<T, MS, allocator_type>::max_size())
  {
    if constexpr (other.max_size() > max_size())
      check_size(other.size());
    realloc(0);
    a_ = std::move(other.a_);
    b_ = other.b_;
    other.b_ = {};
    return *this;
  }
  template<size_t MS, typename R>
  constexpr Vec &operator=(const Vec<T, MS, R> &other)
  {
    size_type new_size = other.size();
    if constexpr (other.max_size() > max_size())
      check_size(new_size);
    if (new_size > capacity()) {
      realloc(0);
      realloc(new_size);
    }
    else
      shrink(new_size);
    auto copy_src_end = other.begin() + size();
    std::copy(other.begin(), copy_src_end, begin());
    append(copy_src_end, other.end());
  }

  constexpr Vec &operator+=(std::ranges::range auto &rg)
  {
    append_range(rg);
    return *this;
  }

  template<typename Self>
  constexpr decltype(auto) operator+(this Self &&self,
                                     std::ranges::range auto &&rg)
  {
    if constexpr (std::convertible_to<Self &&, Vec &&>) {
      self.append_range(rg);
      return std::move(self);
    }
    else {
      Self ret;
      ret.reserve(self.size() + std::ranges::size(rg));
      ret.append_range(self);
      ret.append_range(rg);
      return ret;
    }
  }

  constexpr iterator begin() noexcept { return b_.begin; }
  constexpr const_iterator begin() const noexcept { return b_.begin; }
  constexpr const_iterator cbegin() const noexcept { return b_.begin; }
  constexpr iterator end() noexcept { return b_.end; }
  constexpr const_iterator end() const noexcept { return b_.end; }
  constexpr const_iterator cend() const noexcept { return b_.end; }

  constexpr decltype(auto) at(this auto &&self, size_type i)
  {
    if (i >= self->size())
      throw std::out_of_range(std::format("at({}) exceeds size {} in {} Uvec",
                                          i, self->size(), tname()));
    return (self->data()[i]);
  }
  constexpr decltype(auto) operator[](this auto &&self, size_type i) noexcept
  {
    assert(i < self.size());
    return (self.data()[i]);
  }
  constexpr decltype(auto) front(this auto &&self) { return (self[0]); }
  constexpr decltype(auto) back(this auto &&self)
  {
    return (self[self.size() - 1]);
  }
  constexpr pointer data() noexcept { return b_.begin; }
  constexpr const_pointer data() const noexcept { return b_.begin; }

  constexpr bool empty() const noexcept { return b_.begin == b_.end; }
  constexpr size_type size() const noexcept { return b_.end - b_.begin; }
  static constexpr size_type max_size() noexcept
  {
    return std::min(kMaxSize, kMaxArraySize<T>);
  }
  constexpr void reserve(size_type cap)
  {
    if (cap > capacity())
      realloc(cap);
  }
  constexpr size_type capacity() const noexcept { return b_.bufend - b_.begin; }
  constexpr void shrink_to_fit() { realloc(size()); }

  constexpr void clear() noexcept { shrink(0); }
  template<typename... Args> reference emplace_back(Args &&...args)
  {
    ensure();
    std::construct_at(b_.end, std::forward<Args>(args)...);
    return *b_.end++;
  }
  constexpr void push_back(const value_type &v) { emplace_back(v); }
  constexpr void push_back(value_type &&v) { emplace_back(std::move(v)); }
  constexpr void pop_back() { shrink(size() - 1); }
  constexpr void swap(Vec &other) noexcept
  {
    using std::swap;
    swap(b_, other.b_);
    if constexpr (!std::is_empty_v<Allocator>)
      swap(a_, other.a_);
  }

  constexpr void resize_uninitialized(size_type new_size)
  {
    if !consteval {
      do_resize(new_size, [](pointer b, pointer e) {
        std::uninitialized_default_construct(b, e);
      });
    }
    else {
      resize(new_size);
    }
  }
  constexpr void resize(size_type new_size)
  {
    if !consteval {
      do_resize(new_size, [](pointer b, pointer e) {
        std::uninitialized_value_construct(b, e);
      });
    }
    else {
      resize(new_size, value_type{});
    }
  }
  constexpr void resize(size_type new_size, const value_type &v)
  {
    do_resize(new_size, [v](pointer b, pointer e) {
      if !consteval {
        std::uninitialized_fill(b, e, v);
      }
      else {
        while (b != e)
          std::construct_at(b++, v);
      }
    });
  }
  template<typename Op> void resize_and_overwrite(size_type count, Op op)
  {
    resize_uninitialized(count + 1);
    resize(std::move(op)(data(), count));
  }

  template<std::random_access_iterator IterIn>
  constexpr void append(IterIn first, IterIn last)
      requires requires { value_type(*first); }
  {
    if (const difference_type num = last - first; num > 0) {
      if (capacity() - size() < size_t(num))
        realloc(size() + num);
      if !consteval {
        std::uninitialized_copy(first, last, b_.end);
        b_.end += num;
      }
      else {
        while (first != last) {
          std::construct_at(b_.end, *first++);
          ++b_.end;
        }
      }
    }
  }
  template<std::random_access_iterator IterIn>
  constexpr void append(IterIn first, size_t num)
      requires requires { value_type(*first); }
  {
    append(first, first + num);
  }

  constexpr void append_range(std::ranges::range auto &&rg)
  {
    size_type rsz = std::ranges::size(rg);
    size_type nsz = size() + rsz;
    if (nsz < rsz) [[unlikely]]
      throw XdrLengthError{std::format("Cannot append_range {} elements in "
                                       "Uvec<{}, {}> of size {}",
                                       rsz, tname(), max_size(), size())};
    // In case we are appending ourselves, reserve space first, then
    // reacquire iterators that may have been invalidated.
    reserve(nsz);
    append(std::ranges::begin(rg), std::ranges::end(rg));
  }

  constexpr std::span<value_type> subspan(size_type offset = 0,
                                          size_type count = std::dynamic_extent)
  {
    size_type new_size = size();
    if (offset >= new_size)
      new_size = 0;
    else
      new_size = std::min(new_size - offset, count);
    if (new_size == 0)
      return {};
    return {data() + offset, new_size};
  }

  // Clear the current Uvec and return a new Uvec (or derived type
  // such as Str) in which the pointers have been reinterpret_casted
  // to a new value_type of the same size and no greater alignment.
  // For instance, you can cast between Uvec<unsigned char> and Str.
  template<typename Vec> requires requires(typename Vec::value_type *p) {
    typename Vec::value_type;
    typename Vec::Buf{.begin = p, .end = p, .bufend = p};
    requires sizeof(typename Vec::value_type) == sizeof(value_type);
    requires alignof(value_type) % alignof(typename Vec::value_type) == 0;
    Vec{typename Vec::Buf{}, allocator_type{}};
  }
  Vec reinterpret() && noexcept
  {
    using new_type = Vec::value_type;
    Vec ret{typename Vec::Buf{
                .begin = reinterpret_cast<new_type *>(b_.begin),
                .end = reinterpret_cast<new_type *>(b_.end),
                .bufend = reinterpret_cast<new_type *>(b_.bufend),
            },
            std::move(a_)};
    b_ = {};
    return ret;
  }

  friend constexpr auto operator<=>(const Vec &a, const Vec &b)
      requires requires(const value_type v) { v <=> v; }
  {
    return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(),
                                                  b.end());
  }
  friend constexpr bool operator==(const Vec &a, const Vec &b)
      requires requires(const value_type v) { v == v; }
  {
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
  }

protected:
  static constexpr const char *tname() { return typeid(T).name(); }

  constexpr void shrink(size_type new_size) noexcept
  {
    if (new_size >= size())
      return;
    pointer new_end = b_.begin + new_size;
    std::destroy(new_end, b_.end);
    b_.end = new_end;
  }

  template<std::ranges::range R> constexpr void do_copy(const R &other)
  {
    size_type new_size = std::ranges::size(other);
    if (new_size > capacity()) {
      // Keep current contents if reserve fails, otherwise delete all
      Vec newbuf;
      newbuf.reserve(new_size);
      *this = std::move(newbuf);
    }
    else
      shrink(new_size);
    auto copy_src_end = other.begin() + size();
    std::copy(other.begin(), copy_src_end, begin());
    append(copy_src_end, other.end());
  }

  constexpr void do_resize(size_type new_size, auto &&init)
      requires requires { init(b_.end, b_.bufend); }
  {
    size_type old_size = size();
    if (new_size <= old_size)
      shrink(new_size);
    else {
      ensure(new_size - old_size);
      pointer new_end = b_.begin + new_size;
      init(b_.end, new_end);
      b_.end = new_end;
    }
  }

  constexpr void ensure(size_type n = 1)
  {
    if (size_type(b_.bufend - b_.end) >= n)
      return;
    size_type old_cap = capacity();
    size_type new_cap = std::max(size() + n, std::min(2 * old_cap, max_size()));
    // new_cap may exceed max_size(), but then realloc will throw
    realloc(new_cap);
  }

  constexpr void constexpr_realloc(size_type n)
  {
    // There's no need to use anything other than std::allocator at
    // compile time and allocator_type might not even be constexpr.
    // On top of that, all the std::uninitialized_* functions are
    // non-constexpr (even though they could have been), so we need to
    // implement completely different code at compile time.
    std::allocator<value_type> a;
    Buf newb{};
    if (n) {
      size_t new_size = std::min(size(), n);
      newb.begin = a.allocate(n);
      newb.end = newb.begin + new_size;
      newb.bufend = newb.begin + n;
      for (size_t i = 0; i < new_size; ++i)
        std::construct_at(&newb.begin[i], std::move(b_.begin[i]));
    }
    if (b_.begin) {
      for (value_type *p = b_.begin; p != b_.end; ++p)
        std::destroy_at(p);
      a.deallocate(b_.begin, capacity());
    }
    b_ = newb;
  }

  constexpr void realloc(size_type n)
  {
    check_size(n);
#if !BROKEN_P2564R3
    if consteval {
      constexpr_realloc(n);
      return;
    }
#endif // !BROKEN_P2564R3
    Buf newb{};
    if (n) {
      if constexpr (requires { a_.allocate_at_least(n); }) {
        auto ar = a_.allocate_at_least(n);
        newb.end = newb.begin = reinterpret_cast<value_type *>(ar.ptr);
        newb.bufend = newb.begin + ar.count;
      }
      else {
        newb.end = newb.begin = reinterpret_cast<value_type *>(a_.allocate(n));
        newb.bufend = newb.begin + n;
      }
    }

    if (b_.begin) {
      size_t new_size = std::min(size(), n);
      newb.end = newb.begin + new_size;
      [&] noexcept {
        // for simplicity, use noexcept lambda to crash if move throws
        std::uninitialized_move_n(b_.begin, new_size, newb.begin);
      }();
      std::destroy(b_.begin, b_.end);
      a_.deallocate(reinterpret_cast<allocator_type::value_type *>(b_.begin),
                    b_.bufend - b_.begin);
    }

    b_ = newb;
  }

  constexpr void check_size(size_type n)
  {
    if (n > max_size()) [[unlikely]]
      throw XdrLengthError{std::format("{} exceeds max size in Uvec<{}, {}>", n,
                                       tname(), max_size())};
  }
};

template<typename IterIn>
Vec(IterIn, IterIn) -> Vec<typename std::iterator_traits<IterIn>::value_type>;

template<typename T> Vec(std::initializer_list<T>) -> Vec<T>;

template<typename T> Vec(size_t, const T &) -> Vec<T>;

//! Concept matching Uvec types and types derived from Uvec.
template<typename T>
concept is_vec = std::derived_from<
    T, Vec<typename T::value_type, T::kMaxSize, typename T::allocator_type>>;

// A generalization of std::string_view that applies to Vec and
// std::span of 1-byte trivial types.
template<typename T>
concept is_bytespan = std::ranges::contiguous_range<T> &&
                      std::is_trivial_v<typename T::value_type> &&
                      sizeof(typename T::value_type) == 1;

// Vectors of byte-like types that can be extended with the
// resize_and_overwrite method.  Applies to std::string as well as Vec
// of byte types and Str.
template<typename T>
concept is_bytevec = is_bytespan<T> && requires(T t) {
  t.resize_and_overwrite(1, [](void *, size_t) { return 0; });
};

// Simple byte buffer using Vec.
using Buf = Vec<std::uint8_t>;

// Analogous to std::string_view, but can be initialized from a
// variety of containers of std::uint8_t or char.
struct BufView : std::span<const std::uint8_t> {
  using super = std::span<const std::uint8_t>;
  BufView() noexcept : super() {}
  BufView(is_bytespan auto s) noexcept
    : super(reinterpret_cast<const std::uint8_t *>(s.data()), s.size())
  {}
  BufView(const void *buf, size_t len) noexcept
    : super(static_cast<const std::uint8_t *>(buf), len)
  {}
  BufView &operator=(is_bytespan auto s)
  {
    super::operator=(
        span{reinterpret_cast<const std::uint8_t *>(s.data()), s.size()});
    return *this;
  }
  std::string_view string_view() const
  {
    return {reinterpret_cast<const char *>(data()), size()};
  }
};

template<size_t MaxSize = kMaxArraySize<char> - 1>
class BoundedStr : public Vec<char, MaxSize> {
public:
  using super = Vec<char, MaxSize>;
  using super::super;
  using typename super::size_type;

  static constexpr size_type npos = std::string_view::npos;

  BoundedStr(std::nullptr_t) = delete;
  constexpr BoundedStr(const char *s) : BoundedStr(s, std::strlen(s)) {}
  constexpr BoundedStr(const char *s, size_t count)
  {
    this->resize_uninitialized(count);
    std::copy_n(s, count, this->data());
  }

  constexpr ~BoundedStr() {}

  // Warning: non-const, non-reentrant
  constexpr const char *c_str(this auto &self)
  {
    self.ensure();
    if (*self.b_.end)
      *self.b_.end = '\0';
    return self.data();
  }

  constexpr operator std::string_view() const
  {
    return {this->begin(), this->end()};
  }

  template<typename Self>
  constexpr Self substr(this Self &&self, size_type pos = 0,
                        size_type count = npos)
  {
    size_type new_size = self.size();
    if (pos >= new_size)
      new_size = 0;
    else
      new_size = std::min(new_size - pos, count);

    if (!new_size)
      return Self{};

    if constexpr (std::convertible_to<Self, BoundedStr &&>) {
      if (pos == 0) {
        self.resize(new_size);
        return std::move(self);
      }
      if constexpr (std::is_trivial_v<typename Self::value_type>) {
        memmove(self.data(), self.data() + pos,
                new_size * sizeof(typename Self::value_type));
        self.resize(new_size);
        return std::move(self);
      }
    }

    return Self{self.data() + pos, new_size};
  }

  friend constexpr std::strong_ordering operator<=>(const BoundedStr &a,
                                                    const BoundedStr &b)
  {
    // To make this consistent with char_traits, treat as unsigned.
    const std::uint8_t *ao = reinterpret_cast<const std::uint8_t *>(a.data()),
                       *bp = reinterpret_cast<const std::uint8_t *>(b.data());
    return std::lexicographical_compare_three_way(ao, ao + a.size(), bp,
                                                  bp + b.size());
  }

#define PASSTHROUGH(method)                                                    \
  template<typename... T>                                                      \
  auto method(T &&...t) const noexcept(                                        \
      noexcept(std::string_view{}.method(std::forward<T>(t)...)))              \
      requires requires(std::string_view sv) {                                 \
        sv.method(std::forward<T>(t)...);                                      \
      }                                                                        \
  {                                                                            \
    return std::string_view(*this).method(std::forward<T>(t)...);              \
  }
  PASSTHROUGH(compare)
  PASSTHROUGH(starts_with)
  PASSTHROUGH(ends_with)
  PASSTHROUGH(contains)
  PASSTHROUGH(find)
  PASSTHROUGH(rfind)
  PASSTHROUGH(find_first_of)
  PASSTHROUGH(find_last_of)
  PASSTHROUGH(find_first_not_of)
  PASSTHROUGH(find_last_not_of)
#undef PASSTHROUGH
};

using Str = BoundedStr<>;

constexpr Str
operator""_ustr(const char *s, size_t len)
{
  return Str{s, len};
}

namespace detail {

// Fake output iterator that counts items but does nothing else
struct outcounter {
  std::size_t n{};
  using difference_type = std::ptrdiff_t;
  constexpr auto operator*() const { return std::ignore; }
  constexpr outcounter &operator++() { return ++n, *this; }
  constexpr outcounter operator++(int) { return outcounter{n++}; }
};
static_assert(std::output_iterator<outcounter, char>);

inline size_t
vformatted_size(std::string_view fmt, std::format_args args)
{
  return vformat_to(outcounter{}, fmt, args).n;
}

} // namespace detail

inline Str
uvformat(std::string_view fmt, std::format_args args)
{
  Str ret;
  ret.resize_uninitialized(detail::vformatted_size(fmt, args));
  std::vformat_to(ret.data(), fmt, args);
  return ret;
}

template<typename... Args>
Str
uformat(std::format_string<Args...> fmt, Args &&...args)
{
  return uvformat(fmt.get(), std::make_format_args(args...));
}

// This is much faster than using std::format_to with a
// std::back_inserter.
template<is_bytevec Out, typename... Args>
void
uformat_to(Out *s, std::format_string<Args...> fmt, Args &&...args)
{
  size_t old_size = s->size();
  size_t new_size =
      old_size +
      detail::vformatted_size(fmt.get(), std::make_format_args(args...));
  s->resize_and_overwrite(new_size, [&](auto *p0, size_t count) {
    char *p = reinterpret_cast<char *>(p0);
    char *p2 = std::vformat_to(p + old_size, fmt.get(),
                               std::make_format_args(args...));
    return p2 - p;
  });
}

Str
hexdump(const is_range_of<std::uint8_t> auto &r)
{
  Str out;
  constexpr char hexbytes[] = "0123456789abcdef";
  for (auto c : r) {
    out.push_back(hexbytes[c >> 4 & 0xf]);
    out.push_back(hexbytes[c & 0xf]);
  }
  return out;
}

} // namespace x23

// Rely on implicit conversion to string view for formatting
template<size_t MaxSize>
class std::formatter<x23::BoundedStr<MaxSize>>
  : public std::formatter<std::string_view> {
  using std::formatter<std::string_view>::formatter;
};
