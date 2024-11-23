#pragma once

#include <optional>

#include "storage_object.hh"

namespace hogd {

FullStorageLock
upgrade_lock(std::optional<FullStorageLock> const &previous_lock,
             StorageDelta const &new_delta);

std::optional<StorageLockChange>
compute_lock_change(std::optional<FullStorageLock> const &previous_lock,
                    std::optional<FullStorageLock> const &new_lock);

} // namespace hogd
