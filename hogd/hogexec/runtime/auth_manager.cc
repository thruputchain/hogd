#include "hogd/hogexec/runtime/auth_manager.h"

#include <cassert>
#include <set>
#include <stdexcept>

#include "auth.hh"
#include "hogd/hogexec/runtime/auth_context_stack.h"
#include "hogd/hogexec/runtime/auth_entry_matcher.h"
#include "hogd/hogexec/runtime/status.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

AuthManager::AuthManager(SignedTransaction const &stx)
{
  // If a transaction contains multiple auth entries for one address,
  // only the first entry is used.
  // This should be checked in the transaction preflight logic.
  for (auto const &entry : stx.auth_entries) {
    auth_entries_.emplace(entry.authorizing_address,
                          AuthEntryContext{.matcher_ = AuthEntryMatcher(entry),
                                           .auth_state_ = AuthStateMachine{}});
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

  if (!it->second.auth_state_.can_match()) {
    return false;
  }

  auto res = it->second.matcher_.try_match(auth_context_stack.context_);

  if (res) {
    it->second.auth_state_.mark_used();
  }
  return res;
}

AuthEntry const &
AuthManager::get_entry_for(ContractAddress const &address) const
{
  auto it = auth_entries_.find(address);

  assert(it != auth_entries_.end());

  return it->second.matcher_.base_entry_;
}

void
AuthManager::mark_verify_auth_successful(
    ContractAddress const &authorizing_address)
{
  auto it = auth_entries_.find(authorizing_address);

  assert(it != auth_entries_.end());

  it->second.auth_state_.mark_verify_success();
}

void
AuthManager::mark_verify_auth_failed(ContractAddress const &authorizing_address)
{
  auto it = auth_entries_.find(authorizing_address);

  assert(it != auth_entries_.end());

  it->second.auth_state_.mark_verify_fail();
}

bool
AuthManager::has_been_verified(ContractAddress const &authorizing_address)
{
  auto it = auth_entries_.find(authorizing_address);

  assert(it != auth_entries_.end());

  return it->second.auth_state_.has_been_verified();
}

bool
AuthManager::all_in_valid_termination_state() const
{
  for (auto const &[_, context] : auth_entries_) {
    if (!context.auth_state_.in_valid_termination_state()) {
      return false;
    }
  }

  return true;
}

void
AuthStateMachine::mark_used()
{
  using enum AuthState;

  switch (state_) {
  case kUnverifiedUnused:
    state_ = kUnverifiedUsed;
    break;
  case kVerifiedFailedUnused:
    state_ = kVerifiedFailedUsed;
    break;
  case kUnverifiedUsed:
  case kVerifiedSuccess:
  case kVerifiedFailedUsed:
    // no change
    break;
  default:
    throw std::runtime_error("unknown auth state");
  }
}

void
AuthStateMachine::mark_verify_success()
{
  using enum AuthState;

  assert(state_ != kVerifiedFailedUsed && state_ != kVerifiedFailedUnused &&
         state_ != kVerifiedSuccess);

  state_ = kVerifiedSuccess;
}

void
AuthStateMachine::mark_verify_fail()
{
  using enum AuthState;

  assert(state_ != kVerifiedSuccess);

  switch (state_) {
  case kUnverifiedUnused:
    state_ = kVerifiedFailedUnused;
    break;
  case kUnverifiedUsed:
    state_ = kVerifiedFailedUsed;
    break;
  case kVerifiedFailedUsed:
  case kVerifiedFailedUnused:
    // no change
    break;
  default:
    throw std::runtime_error("unknown auth state");
  }
}

bool
AuthStateMachine::can_match() const
{
  return (state_ != AuthState::kVerifiedFailedUsed) &&
         (state_ != AuthState::kVerifiedFailedUnused);
}

bool
AuthStateMachine::in_valid_termination_state() const
{
  return (state_ == AuthState::kVerifiedSuccess) ||
         (state_ == AuthState::kVerifiedFailedUnused);
}

bool
AuthStateMachine::has_been_verified() const
{
  return (state_ == AuthState::kVerifiedFailedUnused ||
          state_ == AuthState::kVerifiedFailedUsed ||
          state_ == AuthState::kVerifiedSuccess);
}

} // namespace hogd
