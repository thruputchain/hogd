#pragma once

#include <cstddef>
#include <span>

#include "event.hh"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize_cxs.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// Utilities for handling events

// event must be at most length kMaxEventLength
TxEmittedEvent make_tx_emitted_event(ContractAddress const &contract_address,
                                     uint32_t num_topics,
                                     std::span<const std::byte> event);

// Builds an event buffer in the layout consumed by the kLogEvent syscall (and
// by emit_event<N>): a 32-byte signature as topic 0, followed by the event
// struct serialized in CXS form. Any leading fixed-width 32-byte fields (e.g.
// ContractAddress) serialize to raw 32-byte chunks and become the remaining
// topics; trailing fields (e.g. an int128 amount) form the CXS-encoded data
// payload.
//
// This matches the on-the-wire encoding produced by the SDK's
// `#[derive(Event)]` (signature topic + raw 32-byte address topics + a
// CXS-encoded payload), so native contracts emit events that are byte-identical
// to the canonical wasm contracts.
x23::Buf
make_event_from_cxs_struct(Topic const &signature,
                           x23::is_xdr_struct auto const &t)
{
  x23::Buf buf;
  buf.append_range(signature);
  x23::serialize_cxs_to(buf, t);
  return buf;
}
} // namespace hogd
