#include "hogd/hog_event/utilities.h"

#include <cstddef>
#include <span>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

TxEmittedEvent
make_tx_emitted_event(ContractAddress const &contract_address,
                      uint32_t num_topics, std::span<const std::byte> event)
{
  CHECK_LE(event.size(), size_t{kMaxEventLength});

  return TxEmittedEvent{
      .contract_address = contract_address,
      .num_topics = num_topics,
      .serialized_event = x23::Vec<uint8_t, 0xffffffff>(
          reinterpret_cast<uint8_t const *>(event.data()),
          reinterpret_cast<uint8_t const *>(event.data() + event.size())),
  };
}

} // namespace hogd
