#include "hogd/hog_indexer/event_cursor.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "event_cursor_internal.hh"
#include "hogd/crypto/crypto.h"
#include "indexer.hh"
#include "pagination.hh"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd::cursor {
namespace {

void
canonicalize_set(auto &values)
{
  std::ranges::sort(values);
  const auto duplicates = std::ranges::unique(values);
  values.erase(duplicates.begin(), duplicates.end());
}

} // namespace

SHA256Hash
event_query_hash(EventFilterList const &filters)
{
  auto canonical = filters;
  for (auto &filter : canonical) {
    canonicalize_set(filter.contract_addresses);
    canonicalize_set(filter.topics.topic0);
    canonicalize_set(filter.topics.topic1);
    canonicalize_set(filter.topics.topic2);
  }
  canonicalize_set(canonical);
  return sha256(canonical);
}

SHA256Hash
address_query_hash(ContractAddress const &address)
{
  return sha256(address);
}

std::string
encode(EventCursorPayload payload)
{
  payload.schema_version = kCursorVersion;
  auto buf = x23::serialize(payload);
  return std::string(reinterpret_cast<const char *>(buf.data()), buf.size());
}

std::optional<EventCursorPayload>
decode(std::string_view bytes)
{
  if (bytes.empty() || bytes.size() > kMaxCursorBytes) {
    return std::nullopt;
  }
  EventCursorPayload payload;
  // Only XdrException is caught: the payload schema is fixed-size
  // (no Vec / opaque<>), so x23::deserialize doesn't allocate and
  // can't throw bad_alloc.  If a future schema change adds a
  // variable-length field, broaden this catch to std::exception so
  // an allocation failure surfaces as a rejected cursor rather than
  // killing the gateway handler.
  try {
    x23::deserialize(&payload,
                     x23::BufView{reinterpret_cast<const std::uint8_t *>(
                                      bytes.data()),
                                  bytes.size()});
  } catch (x23::XdrException const &) {
    return std::nullopt;
  }
  if (payload.schema_version != kCursorVersion) {
    return std::nullopt;
  }
  switch (payload.method) {
  case HistoryCursorMethod::CursorEvents:
    break;
  case HistoryCursorMethod::CursorTransactions:
  case HistoryCursorMethod::CursorAddressTransactions:
    if (payload.sequence_number != 0) {
      return std::nullopt;
    }
    break;
  default:
    return std::nullopt;
  }
  if (payload.order != IterationOrder::IterationOrderAscending &&
      payload.order != IterationOrder::IterationOrderDescending) {
    return std::nullopt;
  }
  if (payload.block_height < 0 || payload.max_block_height < 0 ||
      payload.block_height > payload.max_block_height) {
    return std::nullopt;
  }
  return payload;
}

} // namespace hogd::cursor
