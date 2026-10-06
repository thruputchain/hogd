#include "hogd/hog_exec/components/auth_token_registry.h"

#include <cassert>
#include <limits>
#include <optional>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/rc.h"
#include "hogd/base/types.h"
#include "hogd/hog_exec/components/auth_subtoken.h"
#include "types.hh"

namespace hogd {

std::optional<uint32_t>
AuthTokenRegistry::register_auth_token(ContractAddress const &mint_address,
                                       BlockHeight expiry_height,
                                       Rc<AuthSubtoken> root)
{
  if (next_token_id_ == std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  uint32_t token_id = next_token_id_++;
  [[maybe_unused]] auto [_, success] = auth_tokens_.emplace(
      token_id, AuthTokenEntry{.token = {.mint_address = mint_address,
                                         .expiry_height = expiry_height,
                                         .node = std::move(root)},
                               .unwrap_result = std::nullopt});
  assert(success);
  return token_id;
}

RegisteredAuthToken const *
AuthTokenRegistry::lookup_auth_token(uint32_t token_id) const
{
  auto it = auth_tokens_.find(token_id);
  return it != auth_tokens_.end() ? &it->second.token : nullptr;
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
  auto const &token = entry.token;
  auto const children = token.node->children.nodes();
  if (children.empty()) {
    return AuthTokenUnwrapResult{.new_token_id_start = 0,
                                 .new_token_id_count = 0};
  }
  auto new_token_id_start = next_token_id_;
  uint32_t new_token_id_count = children.size();

  auto [_, overflow] = overflowing_add(next_token_id_, new_token_id_count);
  if (overflow) {
    return std::nullopt;
  }

  next_token_id_ += new_token_id_count;

  // The children are shared with the parent's node, not copied.
  for (uint32_t i = 0; i < new_token_id_count; i++) {
    uint32_t child_token_id = new_token_id_start + i;
    auto [_, success] = auth_tokens_.emplace(
        child_token_id,
        AuthTokenEntry{.token = {.mint_address = token.mint_address,
                                 .expiry_height = token.expiry_height,
                                 .node = children[i]},
                       .unwrap_result = std::nullopt});
    assert(success);
  }
  entry.unwrap_result =
      AuthTokenUnwrapResult{.new_token_id_start = new_token_id_start,
                            .new_token_id_count = new_token_id_count};
  return entry.unwrap_result;
}
} // namespace hogd
