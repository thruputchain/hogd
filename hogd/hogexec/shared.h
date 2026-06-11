#pragma once

#include "hogd/metrics/shared.h"

namespace hogd {

inline void
init_hogexec_metrics()
{
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "successful_proposed_tx_count", "",
                                  Metrics::SampleType::kPerThreadSsizeT);
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "failed_proposed_tx_count", "",
                                  Metrics::SampleType::kPerThreadSsizeT);
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "gas_consumed", "",
                                  Metrics::SampleType::kPerThreadSsizeT);
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kExecutor);
}

} // namespace hogd
