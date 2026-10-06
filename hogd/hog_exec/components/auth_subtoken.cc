#include "hogd/hog_exec/components/auth_subtoken.h"

#include <cstddef>
#include <span>
#include <tuple>
#include <utility>

#include "auth_token.hh"
#include "hogd/base/rc.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

Rc<AuthSubtoken>
decode_auth_subtoken(std::span<const std::byte> bytes, size_t max_depth)
{
  auto root = make_rc<AuthSubtoken>();
  x23::deserialize(root.get(), x23::BufView{bytes}, max_depth);
  return root;
}

std::tuple<ContractAddress, BlockHeight, Rc<AuthSubtoken>>
decode_auth_token(std::span<const std::byte> bytes, size_t max_depth)
{
  // Laid out as AuthToken, with the root node decoded in place.
  std::tuple<ContractAddress, BlockHeight, AuthSubtoken> token;
  x23::deserialize(&token, x23::BufView{bytes}, max_depth);
  return {std::get<0>(token), std::get<1>(token),
          make_rc<AuthSubtoken>(std::move(std::get<2>(token)))};
}

Rc<AuthSubtoken>
make_auth_subtoken(AuthTokenNode const &node)
{
  auto encoded = x23::serialize(node);
  return decode_auth_subtoken(std::as_bytes(std::span(encoded)),
                              x23::deserialization_recursion_limit);
}

} // namespace hogd
