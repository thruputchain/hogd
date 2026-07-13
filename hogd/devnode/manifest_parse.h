// -*-C++-*-

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace hogd {

// try_parse_u64 parses a complete decimal string into a uint64_t, or returns
// std::nullopt on bad input. It caps at INT64_MAX because hogd::json stores
// integers as int64_t, so a manifest value that survives this parse also
// round-trips through JSON. Callers name the offending field and decide the
// policy (usage() for a CLI flag, a manifest error otherwise).
std::optional<std::uint64_t> try_parse_u64(std::string_view value);

} // namespace hogd
