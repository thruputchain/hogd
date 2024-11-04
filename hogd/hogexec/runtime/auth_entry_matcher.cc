#include "hogd/hogexec/runtime/auth_entry_matcher.h"

namespace hogd {

AuthEntryMatcher::AuthEntryMatcher(AuthEntry const &entry)
  : auth_metadata_(entry.metadata)
{
  for (auto const &node : entry.nodes) {
    nodes_.emplace_back(node);
  }
}

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

AuthEntryMatcher::AuthEntryNodeFrame::AuthEntryNodeFrame(
    AuthEntryNode const &node)
  : context_(node.context), is_leaf_(node.children.empty())
{
  for (auto const &child : node.children) {
    children_.emplace_back(child);
  }
}

} // namespace hogd
