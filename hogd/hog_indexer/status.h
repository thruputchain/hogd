#pragma once

#include <expected>
#include <string>
#include <system_error>

#include "hogd/base/status.h"

namespace hogd {

enum class IndexerErrorCode {
  kNone,
  kInternal,
  kDBNotOpen,
  kDBAlreadyExists,
  kDBNotFound,
  kUnsupportedCompressionAlgorithm,
};

class IndexerErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "indexer"; }
  std::string message(int code) const override;
};

class IndexerError : public EnumError<IndexerErrorCode, IndexerErrorCategory> {
public:
  explicit IndexerError(IndexerErrorCode err) : EnumError(err) {}
};

template<typename T> using IndexerStatus = std::expected<T, IndexerError>;

} // namespace hogd
