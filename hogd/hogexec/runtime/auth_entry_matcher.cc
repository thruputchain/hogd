#include "hogd/hogexec/runtime/auth_entry_matcher.h"

namespace hogd {

AuthEntryMatcher::AuthEntryMatcher(AuthEntry const &entry) : base_entry_(entry)
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
AuthEntryMatcher::try_match(std::vector<AuthContext> const &auth_context_stack)
{
  if (auth_context_stack.size() == 0) {
    return false;
  }

  auto advance_iterator = [=](entry_iter_t &entry_iter) {
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
AuthEntryMatcher::check_iter_match(
    entry_iter_t const &entry_iter,
    std::vector<AuthContext> const &auth_context_stack)
{
  size_t auth_context_idx = 0;
  for (auto const &layer : entry_iter) {
    auto const &it = std::get<1>(layer);
    assert(it != std::get<0>(layer)->end());

    auto const &cur_context = *it;

    if (cur_context.context_ == auth_context_stack[auth_context_idx]) {
      auth_context_idx++;
    }

    if (auth_context_idx == auth_context_stack.size()) {
      return (it->is_leaf_);
    }
  }

  return false;
}

AuthEntryMatcher::AuthEntryNodeFrame::AuthEntryNodeFrame(
    AuthEntryNode const &node)
  : context_(node.context), is_leaf_(node.children.empty())
{
  for (auto const &child : node.children) {
    children_.emplace_back(child);
  }
}

} // namespace hogd
