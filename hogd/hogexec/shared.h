#include <cstdint>
#include <cstdlib>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/socket.h>

#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/executor.h"
#include "hogd/http/http.h"
#include "hogd/init.h"
#include "hogd/metrics/shared.h"
#include "x23/sockio.h"

namespace hogd {

inline void
init_metrics()
{
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "successful_proposed_tx_count", "",
                                  Metrics::SampleType::kSizeT);
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "failed_proposed_tx_count", "",
                                  Metrics::SampleType::kSizeT);
  Metrics::instance().init_metric(Metrics::ComponentName::kExecutor,
                                  "gas_consumed", "",
                                  Metrics::SampleType::kSizeT);
  Metrics::instance().mark_done_with_init(Metrics::ComponentName::kExecutor);
}

} // namespace hogd
