// constants.h - compile-time tunables for the database

#pragma once

#include "hogd/base/types.h"

namespace hogd {

// The size of each loglet region in bytes.
inline constexpr size_t kLogletRegionSize = 2_MiB;
// The threshold for allocating items with an indirect reference from the log.
inline constexpr size_t kBigItemThreshold = 64_KiB;
static_assert(kBigItemThreshold <= kLogletRegionSize);
// The size of an on-disk log file in bytes before starting a new one.
inline constexpr size_t kDiskLogFileThreshold = 128_MiB;
// The min fraction of available memory before compaction is automatically run.
inline constexpr float kLowMemoryRatio = 0.1F;
// the min fraction of a loglet's memory utilization before it can be compacted.
inline constexpr float kMinCompactUtilization = 0.8F;
// the fraction of available cores that can be used for BG activity when active.
inline constexpr int kActiveBGCoreFraction = 4;
// the number of locks per core for the ScalableMap.
inline constexpr int kBaseLockOrder = 9; // 512 * cores
// the number of write heads per core for the DiskWriter.
inline constexpr int kBaseWriteHeadOrder = 2; // 4 * cores

} // namespace hogd
