// -*- C++ -*-

#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <unistd.h>

#include "x23/utility.h"

namespace x23 {

// Self-closing file descriptor will be closed as soon as it goes out
// of scope.
class UniqueFd {
public:
  constexpr UniqueFd() = default;
  constexpr explicit UniqueFd(int fd) : fd_(fd) {}
  UniqueFd(UniqueFd &&uf) : fd_(uf.release()) {}
  ~UniqueFd() { clear(); }
  UniqueFd &operator=(UniqueFd &&uf)
  {
    reset(uf.release());
    return *this;
  }

  // Return the file descriptor number, but maintain ownership.
  int get() const { return fd_; }
  // True if the file descriptor is not -1.
  explicit operator bool() const { return fd_ != -1; }
  // Return the file descriptor number, relinquishing ownership of
  // it.  The \c unique_fd will have file descriptor -1 after this
  // method returns.
  int release()
  {
    int ret = fd_;
    fd_ = -1;
    return ret;
  }
  void clear()
  {
    if (fd_ != -1) {
      close(fd_);
      fd_ = -1;
    }
  }
  void reset(int fd)
  {
    clear();
    fd_ = fd;
  }

private:
  int fd_{-1};
};

// For functions that can take either a UniqueFd or just an integer.
class BorrowedFd {
public:
  BorrowedFd(int fd) : fd_(fd) {}
  BorrowedFd(const UniqueFd &uf) : fd_(uf.get()) {}
  int get() const { return fd_; }

private:
  int fd_{-1};
};

// Managed pointers for C data structures that have an explicit free
// function.  The first template argument is the free function.
// ManagedPtr is like a unique_ptr, except:  1) It automatically
// converts itself to an ordinary pointer to facilitate passing to C
// functions.  2) It doesn't worry about upcasting since C doesn't
// have inheritance.  3) It doesn't support a const version, since
// most C destructor functions require a pointer to the non-const
// structure, unlike C++ which allows deleting const pointers.  4)
// the operator& method returns something that can be cast to an
// element_type** for C functions that want to allocate and return an
// object by assigning it to a pointer.
template<auto Destructor,
         typename T =
             std::remove_pointer_t<ArgNumType<0, decltype(Destructor)>>>
requires requires(T *tp) { Destructor(tp); }
class ManagedPtr {
public:
  using element_type = T;
  using pointer = element_type *;
  static constexpr auto deleter = Destructor;

  ManagedPtr(std::nullptr_t) noexcept {}
  explicit ManagedPtr(pointer p) noexcept : p_(p) {}
  ManagedPtr(const ManagedPtr &) = delete;
  ManagedPtr(ManagedPtr &&other) noexcept : p_(other.release()) {}
  ~ManagedPtr()
  {
    if (p_)
      deleter(p_);
  }

  ManagedPtr &operator=(std::nullptr_t)
  {
    reset(nullptr);
    return *this;
  }
  ManagedPtr &operator=(ManagedPtr &&other) noexcept
  {
    reset(other.release());
    return *this;
  }

  explicit operator bool() const noexcept { return p_; }

  pointer get() const noexcept { return p_; }
  operator pointer() const noexcept { return p_; }
  pointer operator->() const noexcept { return p_; }
  element_type &operator*() const noexcept { return *p_; }

  pointer release() noexcept
  {
    auto r = p_;
    p_ = nullptr;
    return r;
  }
  void reset(pointer p)
  {
    auto old = p_;
    p_ = p;
    if (old)
      deleter(old);
  }

  struct ptrptr_t {
    ManagedPtr *mp_;
    operator pointer *() const { return &mp_->p_; }
    operator ManagedPtr *() const { return mp_; }
  };
  ptrptr_t operator&() noexcept { return {this}; }

  void swap(ManagedPtr &other) noexcept { std::swap(p_, other.p_); }

protected:
  pointer p_{};
};

// Like a managed pointer, but with a default constructor given by
// Allocator.
template<auto Allocator, auto Destructor,
         typename T = std::remove_pointer_t<decltype(Allocator())>>
struct DefManagedPtr : ManagedPtr<Destructor, T> {
  using super = ManagedPtr<Destructor, T>;
  using super::super;
  DefManagedPtr() : super(Allocator()) {}
};

// A nullable type can be constructed or assigned from nullptr and
// converts to bool, like pointers.
template<typename T>
concept is_nullable = requires(T t) {
  T{nullptr};
  t = nullptr;
  bool(t);
  requires[]()->bool
  {
    // bool(T{nullptr}) should be false, but only check if constexpr
    if constexpr (requires { std::bool_constant<bool(T{nullptr})>{}; })
      return !bool(T{nullptr});
    return true;
  }
  ();
};

template<typename Action = std::function<void()>>
requires std::same_as<Action, std::remove_cvref_t<Action>>
class [[nodiscard]] Defer {
public:
  using action_type = Action;
  using nullable_type =
      std::conditional_t<is_nullable<action_type>, action_type,
                         std::optional<action_type>>;

  Defer() noexcept = default;
  [[nodiscard]] Defer(action_type a) noexcept : action_(std::move(a)) {}
  [[nodiscard]] Defer(Defer &&other) noexcept : action_(other.release()) {}

  [[nodiscard]] Defer(std::convertible_to<action_type> auto &&a)
    : Defer(action_type{FORWARD(a)})
  {}

  template<std::convertible_to<action_type> T>
  [[nodiscard]] Defer(Defer<T> &&other)
  {
    if (auto a = std::move(other).release())
      action_ = get_action(std::move(a));
  }

  ~Defer() { invoke(std::move(action_)); }

  Defer &operator=(Defer &&other) { return reset(other.release()), *this; }

  nullable_type release()
  {
    if constexpr (std::same_as<nullable_type, action_type>)
      return std::exchange(action_, nullable_type{nullptr});
    else
      return std::exchange(action_, std::nullopt);
  }

  void reset() { invoke(release()); }
  void reset(action_type a) { invoke(std::exchange(action_, std::move(a))); };

  explicit operator bool() const { return bool(action_); }

private:
  nullable_type action_{};

  static decltype(auto) get_action(auto &&a)
  {
    if constexpr (requires { a(); })
      return a;
    else
      return *FORWARD(a);
  }

  static void invoke(nullable_type &&a)
  {
    if (a)
      get_action(std::move(a))();
  }
};

Defer() -> Defer<std::function<void()>>;
template<typename T> Defer(T) -> Defer<T>;

} // namespace x23
