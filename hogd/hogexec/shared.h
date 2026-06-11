#pragma once

#include "hogd/metrics/shared.h"

namespace hogd {

inline void
init_metrics()
{
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "successful_proposed_tx_count", "",
                                  Metrics::SampleType::kSsizeT);
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "failed_proposed_tx_count", "",
                                  Metrics::SampleType::kSsizeT);
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "gas_consumed", "",
                                  Metrics::SampleType::kSsizeT);
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kExecutor);
}

} // namespace hogd
