#pragma once

#include <expected>
#include <string>
#include <system_error>

#include "hogd/base/status.h"

namespace hogd {

enum class EventErrorCode {
  kNone,
  kBadBlockID,
  kExists,
  kInternal,
};

class EventErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "event"; }
  std::string message(int code) const override;
};

class EventError : public EnumError<EventErrorCode, EventErrorCategory> {
public:
  explicit EventError(EventErrorCode err) : EnumError(err) {}
};

template<typename T> using EventStatus = std::expected<T, EventError>;

} // namespace hogd
