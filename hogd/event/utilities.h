#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include "event.hh"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

// Utilities for handling events

// event must be at most length kMaxEventLength
TxEmittedEvent make_tx_emitted_event(ContractAddress const &contract_address,
                                     uint32_t num_topics,
                                     std::span<const std::byte> event);

// Creates an event from an XDR struct, according to the groundhog event
// standard --- which sets the first 32 bytes as the name of the event struct,
// and the remainder as the serialized event.
x23::Buf
make_event_from_xdr_struct(x23::is_xdr_struct auto const &t)
{
  x23::Buf buf;
  std::array<uint8_t, 32> name_buffer;
  std::memset(name_buffer.data(), 0, name_buffer.size());
  constexpr static std::string_view kTypeName =
      x23::xdr_meta<std::decay_t<decltype(t)>>::type_name;
  std::memcpy(name_buffer.data(), kTypeName.data(), kTypeName.size());
  buf.append_range(name_buffer);
  buf.append_range(x23::serialize(t));
  return buf;
}
} // namespace hogd
