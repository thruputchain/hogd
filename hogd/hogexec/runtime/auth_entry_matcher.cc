#include "hogd/hogexec/runtime/auth_entry_matcher.h"

namespace hogd {

AuthEntryMatcher::AuthEntryMatcher(AuthEntry const &entry)
  : auth_metadata_(entry.metadata)
{
  for (auto const &node : entry.nodes) {
    nodes_.emplace_back(node);
  }

  // initialize iterator
  if (!nodes_.empty()) {
    entry_iterator_.emplace_back(&nodes_, nodes_.begin());

    extend_iterator_to_leaf(entry_iterator_);
  }
}

bool
AuthEntryMatcher::try_match(AuthContextStack const &auth_context_stack)
{
  if (auth_context_stack.context_.size() == 0) {
    return false;
  }

  auto advance_iterator = [=] (entry_iter_t &entry_iter) {
    // advance forward to a subtree containing a leaf
    while (true) {
      if (entry_iter.empty()) {
        return;
      }

      auto &vec = *std::get<0>(entry_iter.back());
      auto &it = std::get<1>(entry_iter.back());

      if (it == vec.end()) {
        entry_iter.pop_back();
      }
      else {
        it++;
        if (it != vec.end()) {
          break;
        }
      }
    }

    extend_iterator_to_leaf(entry_iter);
  };

  while (true) {
    if (entry_iterator_.empty()) {
      // exhausted
      return false;
    }
    // TODO(geoff): cache partial match results

    if (check_iter_match(entry_iterator_, auth_context_stack)) {
      auto &it = std::get<1>(entry_iterator_.back());
      assert(it->is_leaf_);
      if (!it->consumed_) {
        it->consumed_ = true;
        advance_iterator(entry_iterator_);
        return true;
      }
    }
    advance_iterator(entry_iterator_);
  }
}

void
AuthEntryMatcher::extend_iterator_to_leaf(entry_iter_t &entry_iter)
{
  assert(!entry_iter.empty());
  while (true) {
    auto &it = std::get<1>(entry_iter.back());

    assert(it != std::get<0>(entry_iter.back())->end());

    if (it->is_leaf_) {
      return;
    }
    entry_iter.emplace_back(&it->children_, it->children_.begin());
  }
}

bool
AuthEntryMatcher::check_iter_match(entry_iter_t const &entry_iter,
                                   AuthContextStack const &auth_context_stack)
{
  size_t auth_context_idx = 0;
  size_t last_match_idx = 0;
  for (auto const &layer : entry_iter) {
    auto const &it = std::get<1>(layer);
    assert(it != std::get<0>(layer)->end());


    auto const &cur_context = *it;
    
    if (cur_context.context_ ==
        auth_context_stack.context_[auth_context_idx]) {
      auth_context_idx++;
    }

    if (auth_context_idx == auth_context_stack.context_.size()) {
      return (it -> is_leaf_);
    }
  }

  return false;
}

/*
bool
AuthEntryMatcher::try_match(AuthContextStack const &auth_context_stack)
{
  if (auth_context_stack.context_.size() == 0) {
    return true;
  }

  // Essentially just DFS over the auth context tree
  using children_t = std::vector<AuthEntryNodeFrame>;
  using iter_t = children_t::iterator;

  // pair of (children of current node) (did current node match a context frame)
  std::vector<std::tuple<children_t *, iter_t, bool>> explore_nodes;
  explore_nodes.emplace_back(&nodes_, nodes_.begin(), false);

  size_t match_depth = 0;

  while (true) {

    assert(auth_context_stack.context_.size() < match_depth);

    assert(!explore_nodes.empty());

    auto &vec = *std::get<0>(explore_nodes.back());
    auto &it = std::get<1>(explore_nodes.back());
    bool parent_matched = std::get<2>(explore_nodes.back());

    // if current explore is exhausted, pop to parent
    if (it == vec.end()) {
      if (parent_matched) {
        match_depth--;
      }
      explore_nodes.pop_back();

      // boost parent iterator
      if (explore_nodes.empty()) {
        return false;
      }

      std::get<1>(explore_nodes.back())++;
      continue;
    }

    bool found_match = false;

    if (it->context_ == auth_context_stack.context_[match_depth]) {
      match_depth++;
      found_match = true;
    }

    if (match_depth == auth_context_stack.context_.size()) {
      if (it->is_leaf_ && !it->consumed_) {
        it->consumed_ = true;
        return true;
      }
      // otherwise, there's no point to searching the rest of this subtree
      match_depth--;
      it++;
      continue;
    }

    explore_nodes.emplace_back(&(it->children_), it->children_.begin(),
                               found_match);
  }
}
*/

AuthEntryMatcher::AuthEntryNodeFrame::AuthEntryNodeFrame(
    AuthEntryNode const &node)
  : context_(node.context), is_leaf_(node.children.empty())
{
  for (auto const &child : node.children) {
    children_.emplace_back(child);
  }
}

} // namespace hogd
