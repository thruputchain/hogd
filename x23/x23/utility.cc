
#include <unistd.h>

#include "x23/utility.h"

namespace x23 {

static std::string progname;

// Set argv0 for debug logging purposes, in case it can't be inferred
void
setprogname(std::string_view argv0)
{
  if (auto p = argv0.find_last_of('/'); p != argv0.npos)
    progname = std::string(argv0.begin() + p + 1, argv0.end());
  else
    progname = argv0;
}

// Get short program name
const std::string &
getprogname()
{
  if (progname.empty()) {
#if HAVE_GETPROGNAME
    progname = ::getprogname();
#else // !HAVE_GETPROGNAME
    std::string buf(1024UZ, '\0');
    for (;;) {
      auto n = readlink("/proc/self/exe", buf.data(), buf.size());
      if (size_t(n) == buf.size()) {
	buf.resize(2 * buf.size());
	continue;
      }
      if (n > 0)
	setprogname({buf.data(), size_t(n)});
      break;
    }
#endif // !HAVE_GETPROGNAME
  }
  return progname;
}

} // namespace x23
