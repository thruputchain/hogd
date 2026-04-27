#include "hogd/indexer/event_cursor.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "event_cursor_internal.hh"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd::cursor {

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
  return payload;
}

} // namespace hogd::cursor
