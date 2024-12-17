// constants.h - compile-time tunables for the database

#pragma once

#include "hogd/base/types.h"

namespace hogd {

// The size of each loglet region in bytes.
inline constexpr size_t kLogletRegionSize = 4_MiB;
// The threshold for allocating items with an indirect reference from the log.
inline constexpr size_t kBigItemThreshold = 64_KiB;
// The min fraction of available memory before compaction is automatically run.
inline constexpr float kLowMemoryRatio = 0.1F;
// the min fraction of a loglet's memory utilization before it can be compacted.
inline constexpr float kMinCompactUtilization = 0.8F;
// the fraction of available cores that can be used for BG activity when active.
inline constexpr int kActiveBGCoreFraction = 4;

} // namespace hogd
