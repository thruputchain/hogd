#include "hogd/event/event.h"

#include <cassert>
#include <cstddef>
#include <span>

#include "event.hh"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/serialize.h"

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

} // namespace hogd
