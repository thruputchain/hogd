// config_failure.cc - report misconfiguration instead of aborting

#include "hogd/base/config_failure.h"

#include <cstdlib>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <system_error>

#include "hogd/base/log.h"
#include "x23/sockio.h"

namespace hogd {

namespace {

std::terminate_handler previous_terminate_handler = nullptr;

[[noreturn]] void
terminate_on_config_failure()
{
  if (std::optional<std::string> message =
          config_failure_message(std::current_exception())) {
    critical("{}", *message);
    detail::logger->flush();
    // _Exit rather than exit: other threads are still running, so static
    // destructors and atexit handlers are not safe to run.
    std::_Exit(EXIT_FAILURE);
  }
  if (previous_terminate_handler != nullptr)
    previous_terminate_handler();
  std::abort();
}

} // namespace

std::optional<std::string>
config_failure_message(std::exception_ptr e)
{
  if (!e)
    return std::nullopt;
  try {
    std::rethrow_exception(e);
  } catch (const std::system_error &err) {
    if (err.code() == std::errc::address_in_use) {
      return std::format("{}. Is another instance already running, or is "
                         "another service using that port?",
                         err.what());
    }
    if (err.code().category() == x23::gai_category()) {
      return std::format("{}. Check the address, or give a numeric IP.",
                         err.what());
    }
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

void
install_config_failure_handler()
{
  // Installing twice would make the handler its own fallback.
  if (std::get_terminate() == &terminate_on_config_failure)
    return;
  previous_terminate_handler = std::set_terminate(&terminate_on_config_failure);
}

} // namespace hogd
