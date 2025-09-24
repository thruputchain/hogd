#pragma once

#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db_status.h"
#include <span>

namespace hogd {

class HogdbInvariant {
public:
    virtual bool check() = 0;
    virtual void log_item_change(std::span<const std::byte> key, HogDBStatus<DBItem> const& before, HogDBStatus<DBItem> const& after) = 0;

    virtual ~HogdbInvariant() = default;

private:
};

}