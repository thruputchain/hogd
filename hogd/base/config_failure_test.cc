#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "hogd/base/config_failure.h"
#include "hogd/base/network.h"
#include "x23/cleanup.h"
#include "x23/sockio.h"
#include "x23/tests/death_test.h"

namespace hogd {

namespace {

// Listens on a kernel-chosen loopback port, which stays taken while the
// returned fd is open.
x23::UniqueFd
listen_on_loopback()
{
  return x23::tcplisten(*x23::HostPort("127.0.0.1", 0).to_addr());
}

// Returns what listening on the already-taken loopback [port] throws.
std::exception_ptr
bind_error(std::uint16_t port)
{
  try {
    x23::UniqueFd second =
        x23::tcplisten(*x23::HostPort("127.0.0.1", port).to_addr());
  } catch (...) {
    return std::current_exception();
  }
  return nullptr;
}

// Returns what resolving a peer address that cannot exist throws (.invalid is
// reserved by RFC 2606 and never resolves).
std::exception_ptr
resolve_error()
{
  try {
    resolve_host_port(x23::HostPort("unresolvable.invalid", 1));
  } catch (...) {
    return std::current_exception();
  }
  return nullptr;
}

// Lets [e] escape a thread uncaught, as the daemons' startup code lets it
// escape main(), so that it reaches std::terminate.
void
escape_uncaught(std::exception_ptr e)
{
  std::thread([e] { std::rethrow_exception(e); }).join();
}

} // namespace

TEST(ConfigFailure, PortInUseIsReported)
{
  x23::UniqueFd taken = listen_on_loopback();
  const std::uint16_t port = x23::get_port(x23::local_addr(taken));

  const std::string message =
      config_failure_message(bind_error(port)).value_or("(not reported)");
  EXPECT_TRUE(message.contains(std::format("bind(127.0.0.1:{})", port)))
      << message;
  EXPECT_TRUE(message.contains("Is another instance already running"))
      << message;
}

TEST(ConfigFailure, UnresolvableAddressIsReported)
{
  const std::string message =
      config_failure_message(resolve_error()).value_or("(not reported)");
  EXPECT_TRUE(message.contains("resolve unresolvable.invalid:1")) << message;
  EXPECT_TRUE(message.contains("give a numeric IP")) << message;
}

TEST(ConfigFailure, OtherExceptionsAreNotReported)
{
  const std::runtime_error bug("bug");
  const std::system_error refused(std::make_error_code(
                                      std::errc::connection_refused),
                                  "connect");

  EXPECT_FALSE(config_failure_message(nullptr).has_value());
  EXPECT_FALSE(
      config_failure_message(std::make_exception_ptr(bug)).has_value());
  EXPECT_FALSE(
      config_failure_message(std::make_exception_ptr(refused)).has_value());
}

// hogd_init (run by this binary's main) installs the handler, so these
// exercise the same path as a daemon whose startup throws.
TEST(ConfigFailureDeathTest, PortInUseExitsWithoutAborting)
{
  x23::UniqueFd taken = listen_on_loopback();
  std::exception_ptr e = bind_error(x23::get_port(x23::local_addr(taken)));
  ASSERT_NE(e, nullptr);
  EXPECT_EXIT(escape_uncaught(e), testing::ExitedWithCode(EXIT_FAILURE),
              "Address already in use. Is another instance already running");
}

TEST(ConfigFailureDeathTest, UnresolvableAddressExitsWithoutAborting)
{
  std::exception_ptr e = resolve_error();
  ASSERT_NE(e, nullptr);
  EXPECT_EXIT(escape_uncaught(e), testing::ExitedWithCode(EXIT_FAILURE),
              "resolve unresolvable.invalid:1");
}

// Any other uncaught exception is a bug, and must still abort.
TEST(ConfigFailureDeathTest, OtherExceptionsStillAbort)
{
  EXPECT_EXIT(
      {
        x23::test::disable_core_dumps();
        escape_uncaught(std::make_exception_ptr(std::runtime_error("a bug")));
      },
      testing::KilledBySignal(SIGABRT), "a bug");
}

} // namespace hogd
