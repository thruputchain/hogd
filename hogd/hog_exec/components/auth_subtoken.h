#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

#include "auth_token.hh"
#include "hogd/base/rc.h"
#include "types.hh"
#include "x23/utility.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

struct AuthSubtoken;

// The children of an AuthSubtoken, each held by an Rc so that unwrapping a node
// shares its children rather than copying them. x23 treats this as the vector
// AuthTokenNode::child_nodes, and decodes each child straight into a newly
// allocated node.
class AuthSubtokenChildren {
public:
  using value_type = AuthSubtoken;
  static constexpr size_t kMaxSize = kAuthTokenMaxChildNodes;
  static constexpr size_t max_size() { return kMaxSize; }

  class const_iterator;

  AuthSubtokenChildren() = default;
  // Copies share the children.
  AuthSubtokenChildren(AuthSubtokenChildren const &) = default;
  AuthSubtokenChildren &operator=(AuthSubtokenChildren const &) = default;
  AuthSubtokenChildren(AuthSubtokenChildren &&other) noexcept
    : nodes_(std::move(other.nodes_)), size_(std::exchange(other.size_, 0))
  {}
  AuthSubtokenChildren &operator=(AuthSubtokenChildren &&other) noexcept
  {
    nodes_ = std::move(other.nodes_);
    size_ = std::exchange(other.size_, 0);
    return *this;
  }
  ~AuthSubtokenChildren() = default;

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  AuthSubtoken const &operator[](size_t i) const;
  AuthSubtoken &operator[](size_t i);
  const_iterator begin() const;
  const_iterator end() const;

  // The children's Rcs, to share them.
  std::span<Rc<AuthSubtoken> const> nodes() const
  {
    return std::span(nodes_).first(size_);
  }

  // The vector interface x23's decoder uses. As with x23::Vec, growing past
  // kMaxSize throws x23::XdrLengthError.
  void reserve(size_t n) const { check_size(n); }
  void resize(size_t n);
  void resize_uninitialized(size_t n) { resize(n); }

private:
  static void check_size(size_t n);

  std::array<Rc<AuthSubtoken>, kMaxSize> nodes_;
  size_t size_ = 0;
};

// One node of a minted auth token. It has the wire layout of AuthTokenNode, so
// x23 decodes a serialized AuthTokenNode straight into a tree of these.
struct AuthSubtoken {
  ContractAddress consumer_address;
  AuthTokenPayload payload;
  AuthSubtokenChildren children;
};

class AuthSubtokenChildren::const_iterator {
public:
  using value_type = AuthSubtoken;
  using difference_type = std::ptrdiff_t;

  const_iterator() = default;
  explicit const_iterator(Rc<AuthSubtoken> const *p) : p_(p) {}

  AuthSubtoken const &operator*() const { return **p_; }
  const_iterator &operator++()
  {
    ++p_;
    return *this;
  }
  const_iterator operator++(int)
  {
    auto old = *this;
    ++p_;
    return old;
  }
  bool operator==(const_iterator const &) const = default;

private:
  Rc<AuthSubtoken> const *p_ = nullptr;
};

inline AuthSubtoken const &
AuthSubtokenChildren::operator[](size_t i) const
{
  assert(i < size_);
  return *nodes_[i];
}

inline AuthSubtoken &
AuthSubtokenChildren::operator[](size_t i)
{
  assert(i < size_);
  return *nodes_[i];
}

inline AuthSubtokenChildren::const_iterator
AuthSubtokenChildren::begin() const
{
  return const_iterator(nodes_.data());
}

inline AuthSubtokenChildren::const_iterator
AuthSubtokenChildren::end() const
{
  return const_iterator(nodes_.data() + size_);
}

inline void
AuthSubtokenChildren::check_size(size_t n)
{
  if (n > kMaxSize) [[unlikely]] {
    throw x23::XdrLengthError("{} exceeds max size in AuthSubtokenChildren<{}>",
                              n, kMaxSize);
  }
}

inline void
AuthSubtokenChildren::resize(size_t n)
{
  check_size(n);
  for (size_t i = size_; i < n; ++i) {
    nodes_[i] = make_rc<AuthSubtoken>();
  }
  for (size_t i = n; i < size_; ++i) {
    nodes_[i] = Rc<AuthSubtoken>();
  }
  size_ = n;
}

// Decodes a serialized AuthTokenNode into a tree of subtokens, allocating each
// node once. Throws x23::XdrException on malformed input, or on nesting deeper
// than max_depth.
Rc<AuthSubtoken> decode_auth_subtoken(std::span<const std::byte> bytes,
                                      size_t max_depth);

// Decodes a serialized AuthToken: its mint address, expiry height, and root
// node as a tree of subtokens. Throws as decode_auth_subtoken does.
std::tuple<ContractAddress, BlockHeight, Rc<AuthSubtoken>>
decode_auth_token(std::span<const std::byte> bytes, size_t max_depth);

// The subtokens for node, built by encoding it and decoding the result, for
// code that holds an AuthTokenNode rather than its encoding.
Rc<AuthSubtoken> make_auth_subtoken(AuthTokenNode const &node);

// The fields of an AuthSubtoken, in AuthTokenNode's order, as x23 expects of a
// struct.
template<size_t I, x23::references<AuthSubtoken> T> requires (I < 3)
constexpr decltype(auto)
get(T &&t)
{
  if constexpr (I == 0)
    return (std::forward<T>(t).consumer_address);
  if constexpr (I == 1)
    return (std::forward<T>(t).payload);
  if constexpr (I == 2)
    return (std::forward<T>(t).children);
}

} // namespace hogd

template<>
struct std::tuple_size<hogd::AuthSubtoken>
  : std::integral_constant<std::size_t, 3> {};
template<std::size_t I> requires (I < 3)
struct std::tuple_element<I, hogd::AuthSubtoken> {
  using type = decltype(auto(get<I>(hogd::AuthSubtoken{})));
};

template<> struct x23::xdr_meta<hogd::AuthSubtoken> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "AuthTokenNode";
  static constexpr std::array<std::string_view, 3> field_names{
      "consumer_address",
      "payload",
      "child_nodes",
  };
};

template<> struct x23::xdr_meta<hogd::AuthSubtokenChildren> {
  static constexpr x23::XdrCategory category = x23::XdrCategory::kVec;
};
