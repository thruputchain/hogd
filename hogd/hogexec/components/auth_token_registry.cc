#include "hogd/hogexec/components/auth_token_registry.h"

#include <cassert>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "auth_token.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"

namespace hogd {

bool
AuthTokenRegistry::node_within_depth(AuthTokenNode const &root,
                                     uint32_t max_depth)
{
  std::vector<std::pair<AuthTokenNode const *, uint32_t>> stack;
  stack.emplace_back(&root, uint32_t{1});
  while (!stack.empty()) {
    auto [node, depth] = stack.back();
    stack.pop_back();
    if (depth > max_depth) {
      return false;
    }
    if (!node->child_nodes.empty()) {
      if (depth == max_depth) {
        return false;
      }
      for (auto const &child : node->child_nodes) {
        stack.emplace_back(&child, depth + 1);
      }
    }
  }
  return true;
}

std::optional<uint32_t>
AuthTokenRegistry::register_auth_token(AuthToken auth_token)
{
  if (next_token_id_ == std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  uint32_t token_id = next_token_id_++;
  [[maybe_unused]] auto [_, success] =
      auth_tokens_.emplace(token_id, std::move(auth_token));
  assert(success);
  return token_id;
}

AuthToken const *
AuthTokenRegistry::lookup_auth_token(uint32_t token_id) const
{
  auto it = auth_tokens_.find(token_id);
  return it != auth_tokens_.end() ? &it->second.auth_token : nullptr;
}

std::optional<AuthTokenUnwrapResult>
AuthTokenRegistry::unwrap_auth_token(uint32_t token_id)
{
  auto it = auth_tokens_.find(token_id);
  CHECK(it != auth_tokens_.end());
  auto &entry = it->second;
  if (entry.unwrap_result.has_value()) {
    return entry.unwrap_result;
  }
  auto const &auth_token = entry.auth_token;
  auto child_nodes = auth_token.root_node.child_nodes;
  if (child_nodes.empty()) {
    return AuthTokenUnwrapResult{.new_token_id_start = 0,
                                 .new_token_id_count = 0};
  }
  auto new_token_id_start = next_token_id_;
  uint32_t new_token_id_count = child_nodes.size();

  auto [_, overflow] = overflowing_add(next_token_id_, new_token_id_count);
  if (overflow) {
    return std::nullopt;
  }

  next_token_id_ += new_token_id_count;

  for (uint32_t i = 0; i < new_token_id_count; i++) {
    auto child_node = child_nodes[i];
    uint32_t child_token_id = new_token_id_start + i;
    AuthToken child_auth_token =
        AuthToken{.mint_address = auth_token.mint_address,
                  .expiry_height = auth_token.expiry_height,
                  .root_node = child_node};
    auto [_, success] =
        auth_tokens_.emplace(child_token_id, std::move(child_auth_token));
    assert(success);
  }
  entry.unwrap_result =
      AuthTokenUnwrapResult{.new_token_id_start = new_token_id_start,
                            .new_token_id_count = new_token_id_count};
  return entry.unwrap_result;
}
} // namespace hogd
