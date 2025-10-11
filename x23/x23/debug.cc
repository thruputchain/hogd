
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "x23/debug.h"
#include "x23/progname.h"

namespace x23 {

bool ignore_debug_env;

std::string
replace_percent(std::string_view target,
                const std::map<char, std::string> &subst)
{
  std::string out;
  for (size_t i = 0, e = target.size(); i < e; ++i) {
    char c = target[i];
    if (c != '%' || i + 1 == e) {
      out.push_back(c);
      continue;
    }
    c = target[++i];
    if (auto s = subst.find(c); s != subst.end()) {
      out += s->second;
    }
    else {
      if (c != '%')
        out.push_back('%');
      out.push_back(c);
    }
  }
  return out;
}

std::string
expand_log_path(std::string_view path)
{
  static const std::map<char, std::string> expansions = {
      {'f', getprogname()}, {'p', std::to_string(getpid())}};
  return replace_percent(path, expansions);
}

std::string
get_debug_env(const char *var)
{
  if (ignore_debug_env)
    return {};
  if (char *v = getenv(std::format("{}_{}", var, getprogname()).c_str()))
    return v;
  if (char *v = getenv(var))
    return v;
  return {};
}

int
make_debug_fd(const std::string &env_val)
{
  const char *p = env_val.c_str();
  if (const char *comma = std::strrchr(p, ','))
    p = comma + 1;

  if (!*p)
    return -1;

  if (*p == '/' || *p == '.') {
    std::string path = expand_log_path(p);
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd == -1)
      perror(path.c_str());
    return fd;
  }

  char *e;
  if (auto fd = strtoul(p, &e, 10);
      !*e && std::cmp_less_equal(fd, std::numeric_limits<int>::max()))
    return fd;

  return -1;
}

} // namespace x23
