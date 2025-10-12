#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>

#include <unistd.h>

#include "x23/progname.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <vector> // IWYU pragma: keep
#endif

namespace x23 {

static std::string progpath;
static std::string progname;

std::filesystem::path
find_exe(std::filesystem::path prog)
{
  namespace fs = std::filesystem;

  if (prog.empty() || prog.is_absolute())
    return prog;
  if (std::distance(prog.begin(), prog.end()) > 1)
    return fs::absolute(prog);

  std::string_view pathsv{};
  if (const char *p = (std::getenv("PATH")))
    pathsv = p;

  for (auto part : std::views::split(pathsv, ':')) {
    std::filesystem::path dir{std::string_view(part)};
    if (dir.empty())
      dir = ".";
    fs::path exe = dir / prog;
    if (::access(exe.c_str(), X_OK) == 0)
      return exe;
  }
  return prog; // not found
}

#ifdef __APPLE__

// mac-specific utilities for getting path to the current program
std::string
get_executable_path()
{
  uint32_t size = 0;
  // First call with nullptr to get the buffer size
  _NSGetExecutablePath(nullptr, &size);

  std::vector<char> buffer;
  buffer.resize(size);

  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    // Shouldn't happen since we resized
    return {};
  }
  // NOLINTBEGIN(misc-include-cleaner)
  // Canonicalize the path (resolve symlinks, etc.)
  char resolved[PATH_MAX];
  if (realpath(buffer.data(), resolved)) {
    return std::string(resolved);
  }
  // fallback to possibly non-canonical path
  return std::string(buffer.data());
  // NOLINTEND(misc-include-cleaner)
}

#endif

void
setprogpath(std::string_view argv0)
{
  if (auto p = argv0.find_last_of('/'); p != argv0.npos) {
    progpath = argv0;
    progname = std::string(argv0.begin() + p + 1, argv0.end());
  }
  else {
    progname = argv0;
    progpath = find_exe(progname);
  }
}

const std::string &
getprogpath()
{
  if (progpath.empty()) {
#ifdef __APPLE__
    progpath = get_executable_path();
#else  // !HAVE_GETPROGNAME
    std::string buf(1024UZ, '\0');
    for (;;) {
      auto n = readlink("/proc/self/exe", buf.data(), buf.size());
      if (size_t(n) == buf.size()) {
        buf.resize(2 * buf.size());
        continue;
      }
      if (n > 0)
        progpath.assign(buf.data(), n);
      break;
    }
#endif // !HAVE_GETPROGNAME
  }
  return progpath;
}

const std::string &
getprogname()
{
  if (progname.empty())
    progname = std::filesystem::path(getprogpath()).filename().string();
  return progname;
}

} // namespace x23
