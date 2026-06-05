#include "hogd/metrics/new_metrics.h"

namespace hogd {

Metrics &
Metrics::instance()
{
  static Metrics metrics;
  return metrics;
}

} // namespace hogd
