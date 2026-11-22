#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace hogd {

inline bool
makespan_read_diagnostics_enabled()
{
  const char *branch = std::getenv("GIT_BRANCH");
  return branch &&
         std::string_view(branch) == "codex/makespan-read-diagnostics";
}

inline bool
makespan_read_diagnostics_sample(uint64_t read, uint64_t index)
{
  return read > 0 && read <= 1'000'000 &&
         ((index == 0 && read <= 2'000) || read % 1024 == 0);
}

} // namespace hogd
