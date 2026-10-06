#include "hogd/hog_exec/components/auth_token_registry.h"

#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include "auth_token.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/rc.h"
#include "hogd/base/types.h"
#include "types.hh"

namespace hogd {

namespace {

// Builds the subtoken tree for node, moving its payloads out. Children are
// built first, since a node cannot change once built. The recursion is as deep
// as the token, which decoding bounds by max_auth_token_depth.
// NOLINTBEGIN(misc-no-recursion)
Rc<const AuthSubtoken>
make_subtoken(AuthTokenNode &&node, ContractAddress const &mint_address,
              BlockHeight expiry_height)
{
  CHECK_LE(node.child_nodes.size(), size_t{kAuthTokenMaxChildNodes});
  AuthSubtoken subtoken{.mint_address = mint_address,
                        .expiry_height = expiry_height,
                        .consumer_address = node.consumer_address,
                        .payload = std::move(node.payload),
                        .child_count = node.child_nodes.size()};
  for (size_t i = 0; i < subtoken.child_count; ++i) {
    subtoken.child_slots[i] = make_subtoken(std::move(node.child_nodes[i]),
                                            mint_address, expiry_height);
  }
  return make_rc<const AuthSubtoken>(std::move(subtoken));
}
// NOLINTEND(misc-no-recursion)

} // namespace

std::optional<uint32_t>
AuthTokenRegistry::register_auth_token(AuthToken auth_token)
{
  if (next_token_id_ == std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  uint32_t token_id = next_token_id_++;
  [[maybe_unused]] auto [_, success] = auth_tokens_.emplace(
      token_id,
      AuthTokenEntry{.subtoken = make_subtoken(std::move(auth_token.root_node),
                                               auth_token.mint_address,
                                               auth_token.expiry_height),
                     .unwrap_result = std::nullopt});
  assert(success);
  return token_id;
}

AuthSubtoken const *
AuthTokenRegistry::lookup_auth_token(uint32_t token_id) const
{
  auto it = auth_tokens_.find(token_id);
  return it != auth_tokens_.end() ? it->second.subtoken.get() : nullptr;
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
  auto const children = entry.subtoken->children();
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

  // The children are shared with the parent's subtoken, not copied.
  for (uint32_t i = 0; i < new_token_id_count; i++) {
    uint32_t child_token_id = new_token_id_start + i;
    auto [_, success] =
        auth_tokens_.emplace(child_token_id,
                             AuthTokenEntry{.subtoken = children[i],
                                            .unwrap_result = std::nullopt});
    assert(success);
  }
  entry.unwrap_result =
      AuthTokenUnwrapResult{.new_token_id_start = new_token_id_start,
                            .new_token_id_count = new_token_id_count};
  return entry.unwrap_result;
}
} // namespace hogd
