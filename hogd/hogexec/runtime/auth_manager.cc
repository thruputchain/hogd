#include "hogd/hogexec/runtime/auth_manager.h"

#include <set>

#include "auth.hh"
#include "hogd/hogexec/runtime/auth_entry_matcher.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

AuthManager::AuthManager(SignedTransaction const &stx)
{
  // If a transaction contains multiple auth entries for one address,
  // only the first entry is used.
  // This should be checked in the transaction preflight logic.
  for (auto const &entry : stx.auth_entries) {
    auth_entries_.emplace(entry.authorizing_address,
                          AuthEntryContext{.matcher_ = AuthEntryMatcher(entry),
                                           .auth_checked_ = false});
  }
}

bool
AuthManager::initialization_validate_auth_entries(SignedTransaction const &stx)
{
  std::set<ContractAddress> addresses;
  for (auto const &entry : stx.auth_entries) {
    addresses.insert(entry.authorizing_address);
  }
  return addresses.size() == stx.auth_entries.size();
}

bool
AuthManager::try_match(ContractAddress const &address,
                       AuthContextStack const &auth_context_stack)
{
  auto it = auth_entries_.find(address);

  if (it == auth_entries_.end()) {
    return false;
  }

  return it->second.matcher_.try_match(auth_context_stack);
}

AuthEntry const &
AuthManager::get_entry_for(ContractAddress const &address) const
{
  auto it = auth_entries_.find(address);

  assert(it != auth_entries_.end());

  return it->second.matcher_.base_entry_;
}

} // namespace hogd
