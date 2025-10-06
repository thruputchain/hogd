
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>

#include <unistd.h>

#include "x23/config.h"
#include "x23/progname.h"

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
#if HAVE_GETPROGNAME
    progpath = find_exe(::getprogname());
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
