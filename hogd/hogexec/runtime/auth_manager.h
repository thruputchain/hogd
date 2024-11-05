#pragma once

#include <map>

#include "auth.hh"
#include "hogd/hogexec/runtime/auth_entry_matcher.h"
#include "hogd/hogexec/runtime/status.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

enum class AuthState {
  // Initial state: verify_auth not called, and auth has not been used for a
  // match
  //
  // A transaction cannot terminate successfully in this state,
  // but can transition to a valid state by calling verify_auth.
  kUnverifiedUnused = 0,
  // Not yet verified, but auth has been used to match
  //
  // A transaction cannot terminate successfully in this state,
  // but can transition to a valid state by calling verify_auth.
  kUnverifiedUsed = 1,
  // Verified successfully, may or may not have been used for a match
  //
  // A transaction cannot terminate successfully in this state,
  // but can transition to a valid state by calling verify_auth.
  kVerifiedSuccess = 2,
  // Verification failed, but has not been used to match anything
  //
  // A transaction can terminate successfully in this state.
  kVerifiedFailedUnused = 3,
  // Verification, but was earlier used to match.
  //
  // A transaction cannot terminate successfully in this state.
  // No state transition can bring the transaction to a state in which
  // it can successfully terminate.
  kVerifiedFailedUsed = 4,
};

class AuthStateMachine {
public:
  void mark_used();
  void mark_verify_success();
  void mark_verify_fail();

  bool can_match() const;
  bool in_valid_termination_state() const;
  bool has_been_verified() const;

private:
  AuthState state_ = AuthState::kUnverifiedUnused;
};

class AuthManager {
public:
  AuthManager(SignedTransaction const &stx);

  bool try_match(ContractAddress const &address);

  struct AuthEntryContext {
    AuthEntryMatcher matcher_;
    AuthStateMachine auth_state_;
  };

  using AuthContextStack = std::vector<AuthContext>;

  AuthContextStack auth_context_stack_;

  AuthEntry const &get_entry_for(ContractAddress const &address) const;
  bool has_entry_for(ContractAddress const &address) const
  {
    return auth_entries_.find(address) != auth_entries_.end();
  }

  void mark_verify_auth_successful(ContractAddress const &authorizing_address);
  void mark_verify_auth_failed(ContractAddress const &authorizing_address);
  bool has_been_verified(ContractAddress const &authorizing_address);

  static bool
  initialization_validate_auth_entries(SignedTransaction const &stx);

  bool all_in_valid_termination_state() const;

  const auto &get_auth_entries() const { return auth_entries_; }

private:
  std::map<ContractAddress, AuthEntryContext> auth_entries_;
};

} // namespace hogd
