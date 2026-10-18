#include "hogd/hog_auth/dbauth_client.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hog_auth/status.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/rpc.h"
#include "x23/serialize.h"

namespace hogd {
namespace {

AuthStatus<std::optional<AddressAuthData>>
decode_auth_data(DBAuthResult res)
{
  switch (res.error()) {
  case DBAuthError::kDBAuthSuccess:
    return AuthStatus<std::optional<AddressAuthData>>{
        std::move(res.auth_data())};
  case DBAuthError::kDBAuthNotFound:
    return AuthStatus<std::optional<AddressAuthData>>{std::nullopt};
  case DBAuthError::kDBAuthStaleBlockHeight:
    return AuthError(AuthErrorCode::kAuthStaleBlockHeight);
  case DBAuthError::kDBAuthInternal:
    return AuthError(AuthErrorCode::kAuthInternal);
  }
  error("unknown DBAuthError: {}", static_cast<int>(res.error()));
  return AuthError(AuthErrorCode::kAuthInternal);
}

} // namespace

DBAuthClient::DBAuthClient(dbauth_client_t &&client)
  : client_(std::move(client))
{
  // Use the cap enforced by this connection's reader, including clients
  // configured below the default. Allow room for the reply envelope and
  // vector length, and budget for fully populated ML-DSA key sets.
  constexpr size_t kReplyOverhead = 1024;
  const size_t cap = client_.__send_call->max_message_size;
  CHECK_GT(cap, kReplyOverhead);
  max_lookup_batch_size_ =
      std::min(kMaxLookupBatchSize,
               (cap - kReplyOverhead) /
                   x23::detail::xdr_size_bounds<DBAuthResult>().max_);
  CHECK_GT(max_lookup_batch_size_, 0UL);
}

x23::Async<AuthStatus<std::optional<AddressAuthData>>>
DBAuthClient::lookup_auth_data(BlockHeight height, ContractAddress address)
{
  co_return decode_auth_data(
      co_await client_.authdb_get_auth_data(height, address));
}

x23::Async<std::vector<AuthStatus<std::optional<AddressAuthData>>>>
DBAuthClient::lookup_auth_data_batch(BlockHeight height,
                                     const ContractAddressBatch &addresses)
{
  CHECK_LE(addresses.size(), max_lookup_batch_size_);
  std::vector<AuthStatus<std::optional<AddressAuthData>>> result;
  if (addresses.empty()) {
    co_return result;
  }
  auto replies = co_await client_.authdb_get_auth_data_batch(height, addresses);
  result.reserve(addresses.size());
  if (replies.size() != addresses.size()) {
    warn("DB auth batch returned {} results for {} addresses", replies.size(),
         addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
      result.push_back(AuthError(AuthErrorCode::kAuthInternal));
    }
    co_return result;
  }
  for (auto &reply : replies) {
    result.push_back(decode_auth_data(std::move(reply)));
  }
  co_return result;
}

x23::Async<BlockHeight>
DBAuthClient::subscribe_to_auth_data_changes(AuthCard const &card)
{
  DBAuthIntroResult res = co_await client_.dbauth_intro(card);
  CHECK_EQ(res.outcome(), DBAuthIntroOutcome::kIntroSuccess);
  co_return res.current_height();
}

} // namespace hogd
