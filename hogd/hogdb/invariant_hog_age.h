#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>

#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/invariant.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include "x23/mvar.h"

namespace hogd {

class HogAgeInvariant : public HogdbInvariant {
public:
  bool check() override;
  void log_item_change(std::span<const std::byte> key,
                       HogDBStatus<DBItem> const &before,
                       HogDBStatus<DBItem> const &after) override;

private:
  enum class ExistenceDelta : int8_t {
    kDeleted = -1,
    kUnchanged = 0,
    kCreated = 1,
  };

  struct Entry {
    ExistenceDelta balance{ExistenceDelta::kUnchanged};
    ExistenceDelta hog_age{ExistenceDelta::kUnchanged};
  };

  static ExistenceDelta compute_delta(HogDBStatus<DBItem> const &before,
                                      HogDBStatus<DBItem> const &after);

  static RawAddress raw_address_from_key(std::span<const std::byte> key);

  void record_delta(std::span<const std::byte> key,
                    ExistenceDelta delta,
                    ExistenceDelta Entry::*field);

  x23::MVar<std::map<RawAddress, Entry>> entries_;
};

} // namespace hogd
