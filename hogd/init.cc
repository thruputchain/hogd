#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "types.hh"

namespace hogd {

namespace {

// add your init functions here
const std::pair<InitFunc, std::string_view> init_tbl[] = {
    {base_init, "base"},
    {crypto_init, "crypto"},
};

// Reads the binary of the currently running process.
std::vector<std::byte>
read_self_binary()
{
  std::ifstream f("/proc/self/exe", std::ios::binary | std::ios::ate);
  CHECK(f.is_open());

  const std::streamsize len = f.tellg();
  CHECK_GT(len, 0L);

  std::vector<std::byte> buf(static_cast<size_t>(len));
  f.seekg(0, std::ios::beg);
  CHECK(f.good());

  f.read(reinterpret_cast<char *>(buf.data()), len);
  CHECK_EQ(f.gcount(), len);
  CHECK(f.good());
  return buf;
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
print_version(const char *process)
{
  info("hogd version: {}, git commit hash: {}", version::kHogdVersion,
       version::kHogdGitHash);
  info("hogd protocol: {}", version::kHogdProtocol);
  info("hogd process: {}, binary hash: {}", process, get_self_binary_hash());
}

} // namespace

int
hogd_init(int argc, char *argv[])
{
  // TODO(amb): parse arguments and set up configuration

  // initialize logging
  log_init(std::make_unique<ConsoleLogger>());

  // initialize each subsystem
  for (auto [func, name] : init_tbl) {
    debug("Initializing '{}'.", name);
    GStatus<void> ret = func();
    if (!ret) {
      error("Failed to initialize '{}', ret={}", name, ret.error());
      return EXIT_FAILURE;
    }
  }

  CHECK_GT(argc, 0);
  // We deliberately call [print_version] after initializing the subsystems
  // above. [print_version()] uses [sha256()] from the crypto library, so we
  // need to initialize the crypto subsystem first.
  print_version(argv[0]);

  return EXIT_SUCCESS;
}

} // namespace hogd
