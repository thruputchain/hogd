#include "hogd/hogexec/runtime/metrics.h"
#include "hogd/base/log.h"
#include <chrono>
#include <stop_token>
#include <thread>

namespace hogd {

void HogexecMetrics::launch_log_worker()
{
  auto &instance = get_instance();
  if (!instance.background_worker_.joinable()) {
    instance.background_worker_ = std::jthread([] (std::stop_token stoken)
    {
      get_instance().background_worker(stoken);
    });
  }
}

HogexecMetrics &HogexecMetrics::get_instance()
{
  static HogexecMetrics instance;
  return instance;
}

void HogexecMetrics::print_stats()
{
  auto samples = registry_.read();
  for (auto &sample : samples) {
    info("{}: {}", sample.key, sample.val);
  }
}

void HogexecMetrics::background_worker(std::stop_token &stoken)
{
  while (!stoken.stop_requested()) {
    print_stats();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

} // namespace hogd
