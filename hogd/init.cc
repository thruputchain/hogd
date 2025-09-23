#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/init.h"
#include "hogd/version.h"

namespace hogd {

namespace {

// add your init functions here
const std::pair<InitFunc, std::string_view> init_tbl[] = {
    {base_init, "base"},
    {crypto_init, "crypto"},
};

} // namespace

int
hogd_init([[maybe_unused]] int argc, [[maybe_unused]] char *argv[])
{
  // TODO(amb): parse arguments and set up configuration

  // initialize logging
  log_init(std::make_unique<ConsoleLogger>());

  version::print_version();

  // initialize each subsystem
  for (auto [func, name] : init_tbl) {
    debug("Initializing '{}'.", name);
    GStatus<void> ret = func();
    if (!ret) {
      error("Failed to initialize '{}', ret={}", name, ret.error());
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

} // namespace hogd
