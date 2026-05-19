#include <cstdlib>
#include <iomanip>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <x23/debug.h>
#include <x23/fileio.h>
#include <x23/progname.h>
#include <x23/vec.h>

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "types.hh"

#ifdef __APPLE__
#include <cstring>
#include <mach-o/dyld.h>
#endif

std::string cmdline; // NOLINT

namespace hogd {

namespace {

// add your init functions here
const std::pair<InitFunc, std::string_view> init_tbl[] = {
    {base_init, "base"},
    {crypto_init, "crypto"},
};

constexpr std::string_view kJsonLogFormat = "json";

std::unique_ptr<Logger>
make_logger(LogLevel log_level)
{
  const char *log_format = std::getenv("HOGD_LOG_FORMAT");
  if (log_format != nullptr && std::string_view(log_format) == kJsonLogFormat) {
    return std::make_unique<JsonConsoleLogger>(log_level);
  }
  return std::make_unique<ConsoleLogger>(log_level);
}

// Reads the binary of the currently running process.
x23::Buf
read_self_binary()
{
  const std::string &path = x23::getprogpath();
  if (path.empty())
    panic("unable to determine path of binary");
  info("reading self binary from {}", path);
  auto rres = x23::readfile<x23::Buf>(path);
  if (!rres)
    panic("{}: {}", path, rres.error());
  CHECK_GT(rres->size(), 0UZ);
  return *rres;
}

// Generates the hash of the binary for the currently running process.
std::string
get_self_binary_hash()
{
  constexpr size_t kHashLength = 12;
  static_assert(kHashLength % 2 == 0);

  // The number of hex digits to print of the binary hash.
  SHA256Hash binary_hash = sha256(read_self_binary());
  // We multiply by 2 because there are 2 hex digits per byte.
  CHECK_LE(kHashLength, binary_hash.size() * 2);
  std::ostringstream binary_hash_stream;
  for (size_t i = 0; i < kHashLength / 2; i++) {
    binary_hash_stream << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(binary_hash[i]);
  }
  return binary_hash_stream.str();
}

void
print_version(const char *process, bool do_print_binary_hash)
{
  info("hogd version: {}, git commit hash: {}", version::kHogdVersion,
       version::kHogdGitHash);
  info("hogd config: {}", version::kHogdCmake);
  info("hogd protocol: {}", version::kHogdProtocol());
  info("hogd command: {}", cmdline);
  if (do_print_binary_hash)
    info("hogd process: {}, binary hash: {}", process, get_self_binary_hash());
}

} // namespace

int
hogd_init(int argc, char *argv[], bool do_print_version, LogLevel log_level)
{
  // record command line
  cmdline.clear();
  for (int i = 0; i < argc; ++i)
    x23::append_arg(&cmdline, argv[i]);

  // initialize logging
  // NOTE: this must come before any use of the CHECK macros
  log_init(make_logger(log_level));

  // Ensure we have a pathname for our executable
  if (x23::getprogpath().empty()) {
    CHECK_GT(argc, 0);
    x23::setprogpath(argv[0]);
  }

  // initialize each subsystem
  for (auto [func, name] : init_tbl) {
    debug("Initializing '{}'.", name);
    GStatus<void> ret = func();
    if (!ret) {
      error("Failed to initialize '{}', ret={}", name, ret.error());
      return EXIT_FAILURE;
    }
  }

  // We deliberately call [print_version] after initializing the subsystems
  // above. [print_version()] uses [sha256()] from the crypto library, so we
  // need to initialize the crypto subsystem first.
  if (do_print_version) {
    CHECK_GT(argc, 0);
    print_version(argv[0], true);
  }
  else {
    print_version(nullptr, false);
  }

  return EXIT_SUCCESS;
}

} // namespace hogd
