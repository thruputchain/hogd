#pragma once

#include <vector>

#include "auth.hh"

namespace hogd {

// Match auth events for one Address.
// Mirrors the AuthEntry structure.
class AuthEntryMatcher {
public:
  explicit AuthEntryMatcher(AuthEntry const &entry);
  bool try_match(std::vector<AuthContext> const &auth_context_stack);

  const AuthEntry base_entry_;

private:
  struct AuthEntryNodeFrame {
    AuthContext context_;
    std::vector<AuthEntryNodeFrame> children_;
    bool consumed_ = false;
    const bool is_leaf_;

    explicit AuthEntryNodeFrame(AuthEntryNode const &node);
  };

  using children_vector_t = std::vector<AuthEntryNodeFrame>;
  using vector_iter_t = children_vector_t::iterator;
  using entry_iter_t =
      std::vector<std::tuple<children_vector_t *, vector_iter_t>>;

  children_vector_t nodes_;

  entry_iter_t entry_iterator_;

  static bool
  check_iter_match(entry_iter_t const &entry_iter,
                   std::vector<AuthContext> const &auth_context_stack);

  static void extend_iterator_to_leaf(entry_iter_t &entry_iter);
};

} // namespace hogd
