#include "hogd/event/event.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <span>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

TxEmittedEvent
make_tx_emitted_event(ContractAddress const &contract_address,
                      SHA256Hash const *topic0, SHA256Hash const *topic1,
                      SHA256Hash const *topic2,
                      std::span<const std::byte> event)
{
  EventData data{};
  if (topic0) {
    data.topics.push_back(*topic0);
  }
  if (topic1) {
    assert(topic0);
    data.topics.push_back(*topic1);
  }
  if (topic2) {
    assert(topic0);
    assert(topic1);
    data.topics.push_back(*topic2);
  }

  assert(event.size() <= kMaxLogMessageLen);

  data.event = x23::Vec<uint8_t, 0xffffffff>(
      reinterpret_cast<uint8_t const *>(event.data()),
      reinterpret_cast<uint8_t const *>(event.data() + event.size()));
  return TxEmittedEvent{
      .contract_address = contract_address,
      .serialized_event = x23::serialize(data),
  };
}

std::optional<EventData>
try_parse_event_data(TxEmittedEvent const &emitted_event)
{
  EventData data{};
  try {
    x23::deserialize(&data, x23::BufView{emitted_event.serialized_event});
  } catch (const x23::XdrBadInput &e) {
    warn("Failed to parse event data: {}", e.what());
    return std::nullopt;
  }
  return data;
}

} // namespace hogd
