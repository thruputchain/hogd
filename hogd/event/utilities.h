#pragma once

#include <cstddef>
#include <span>

#include "event.hh"
#include "types.hh"

namespace hogd {

// Utilities for handling events

// event must be at most length kMaxLogMessageLen
TxEmittedEvent make_tx_emitted_event(ContractAddress const &contract_address,
                                     SHA256Hash const *topic0,
                                     SHA256Hash const *topic1,
                                     SHA256Hash const *topic2,
                                     std::span<const std::byte> event);

std::optional<EventData>
try_parse_event_data(TxEmittedEvent const &emitted_event);

} // namespace hogd
