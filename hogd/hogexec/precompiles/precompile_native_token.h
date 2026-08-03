#pragma once

#include <memory>

#include "event.hh"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/native_runtime.h"

namespace hogd {

class SystemCallHandler;

// Stage an unchecked native-token debit of `from` and credit of `to`.
// No balance check, no Transfer event: callers on the public ERC20 surface
// must run the strict checks first; the genesis initial-token-claim calls
// this directly because its in-tx balance is transiently short by the
// storage deposit that hogdb's exclusive-delete refund nets at commit.
// `amount` must be nonnegative.
InvokeStatus<void>
native_token_unchecked_transfer(SystemCallHandler &handler,
                                ContractAddress const &from,
                                ContractAddress const &to, int128_t amount);

std::unique_ptr<NativeContract> make_native_token_contract();

// Event-signature topics (topic 0) for the native token's standard asset
// events. These reproduce the SDK's event-signature scheme,
// sha256("<major>::<crate>::<EventName>"), for the `groundhog-erc20-if` crate
// (currently version 0.1.x, so the prefix is "0.1"), so that the native token
// emits events byte-identical to the canonical wasm `itc_erc20` contract.
Topic const &native_token_transfer_event_signature();
Topic const &native_token_approval_event_signature();

} // namespace hogd
