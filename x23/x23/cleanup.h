// -*-C++-*-

#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <unistd.h>

#include "x23/utility.h"

namespace x23 {

// Invalid file descriptor number returned by system calls on error.
constexpr inline int kInvalidFd = -1;

// Self-closing file descriptor will be closed as soon as it goes out
// of scope.
class UniqueFd {
public:
  constexpr UniqueFd() noexcept = default;
  constexpr explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  UniqueFd(UniqueFd &&uf) noexcept : fd_(uf.release()) {}
  ~UniqueFd() { reset(); }
  UniqueFd &operator=(UniqueFd &&uf) noexcept
  {
    reset(uf.release());
    return *this;
  }

  // Return the file descriptor number, but maintain ownership.
  int get() const noexcept { return fd_; }

  // True if UniqueFd is managing a file descriptor.
  explicit operator bool() const noexcept { return fd_ != kInvalidFd; }

  // Return the file descriptor number, relinquishing ownership of it.
  // On return, the UniqueFd no longer manages a file descriptor.
  int release() noexcept { return std::exchange(fd_, kInvalidFd); }

  // Close the managed file descriptor and replace it with fd (or
  // replace it with no file descriptor by default).
  void reset(int fd = kInvalidFd) noexcept
  {
    if (fd_ != kInvalidFd)
      close(fd_);
    fd_ = fd;
  }

private:
  int fd_{kInvalidFd};
};

// For functions that can take either a UniqueFd or just an integer.
class BorrowedFd {
public:
  BorrowedFd(int fd) noexcept : fd_(fd) {}
  BorrowedFd(const UniqueFd &uf) noexcept : fd_(uf.get()) {}
  int get() const noexcept { return fd_; }

private:
  int fd_{kInvalidFd};
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
  ManagedPtr(ManagedPtr &&other) noexcept : p_(other.release()) {}
  ~ManagedPtr()
  {
    if (p_)
      deleter(p_);
  }

  ManagedPtr &operator=(std::nullptr_t) noexcept
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

  pointer release() noexcept { return std::exchange(p_, nullptr); }
  void reset(pointer p) noexcept
  {
    if (auto old = std::exchange(p_, p))
      deleter(old);
  }

  struct PtrPtr {
    ManagedPtr *mp_;
    operator pointer *() const { return &mp_->p_; }
    operator ManagedPtr *() const { return mp_; }
  };
  PtrPtr operator&() noexcept { return {this}; }

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
      noexcept(noexcept(action_type{std::forward<decltype(a)>(a)}))
    : Defer(action_type{std::forward<decltype(a)>(a)})
  {}

  template<std::convertible_to<action_type> T>
  [[nodiscard]] Defer(Defer<T> &&other) noexcept
  {
    if (auto a = std::move(other).release())
      action_ = get_action(std::move(a));
  }

  ~Defer() { invoke(std::move(action_)); }

  Defer &operator=(Defer &&other) noexcept
  {
    reset(other.release());
    return *this;
  }

  nullable_type release() noexcept
  {
    if constexpr (std::same_as<nullable_type, action_type>)
      return std::exchange(action_, nullable_type{nullptr});
    else
      return std::exchange(action_, std::nullopt);
  }

  void reset() { invoke(release()); }
  void reset(action_type a) { invoke(std::exchange(action_, std::move(a))); };

  explicit operator bool() const noexcept { return bool(action_); }

private:
  nullable_type action_{};

  static decltype(auto) get_action(auto &&a) noexcept
  {
    if constexpr (requires { a(); })
      return a;
    else
      return *std::forward<decltype(a)>(a);
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
