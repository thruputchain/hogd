#include "hogd/indexer/event_cursor.h"

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "event.hh"
#include "hogd/base/types.h"
#include "indexer.hh"
#include "pagination.hh"
#include "types.hh"

namespace hogd {

namespace {

void
write_be64(std::span<uint8_t, 8> out, uint64_t v)
{
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

uint64_t
read_be64(std::span<const uint8_t, 8> in)
{
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v = (v << 8) | in[i];
  }
  return v;
}

void
write_be32(std::span<uint8_t, 4> out, uint32_t v)
{
  for (int i = 3; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

uint32_t
read_be32(std::span<const uint8_t, 4> in)
{
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    v = (v << 8) | in[i];
  }
  return v;
}

} // namespace

std::string
EventCursor::encode(IterationOrder order, BlockHeight block_height,
                    SHA256Hash const &tx_hash, uint32_t sequence_number)
{
  std::string out(kEncodedSize, '\0');
  auto *data = reinterpret_cast<uint8_t *>(out.data());
  data[0] = kVersion;
  data[1] = static_cast<uint8_t>(order);
  // BlockHeight is a signed int64; cast through uint64_t preserves
  // bit pattern so negative values round-trip.
  write_be64(std::span<uint8_t, 8>(data + 2, 8),
             static_cast<uint64_t>(block_height));
  std::memcpy(data + 10, tx_hash.data(), 32);
  write_be32(std::span<uint8_t, 4>(data + 42, 4), sequence_number);
  return out;
}

std::optional<EventCursor::Decoded>
EventCursor::decode(std::string_view cursor)
{
  if (cursor.size() != kEncodedSize) {
    return std::nullopt;
  }
  const auto *data = reinterpret_cast<const uint8_t *>(cursor.data());
  if (data[0] != kVersion) {
    return std::nullopt;
  }
  const uint8_t order_byte = data[1];
  if (order_byte !=
          static_cast<uint8_t>(IterationOrder::IterationOrderAscending) &&
      order_byte !=
          static_cast<uint8_t>(IterationOrder::IterationOrderDescending)) {
    return std::nullopt;
  }

  Decoded d;
  d.order = static_cast<IterationOrder>(order_byte);
  d.block_height = static_cast<BlockHeight>(
      read_be64(std::span<const uint8_t, 8>(data + 2, 8)));
  std::memcpy(d.tx_hash.data(), data + 10, 32);
  d.sequence_number = read_be32(std::span<const uint8_t, 4>(data + 42, 4));
  return d;
}

} // namespace hogd
