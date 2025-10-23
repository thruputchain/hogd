#pragma once

#include <cstddef>
#include <span>

#include "event.hh"
#include "types.hh"

namespace hogd {

// Utilities for handling events

// event must be at most length kMaxLogMessageLen
TxEmittedEvent make_tx_emitted_event(ContractAddress const &contract_address,
                                     uint32_t num_topics,
                                     std::span<const std::byte> event);

} // namespace hogd
