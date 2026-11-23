#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string_view>

#include "hogd/base/log.h"
#include "hogd/version.h"

namespace hogd::indexer_phase {

inline std::string_view
env(const char *name)
{
  const char *value = std::getenv(name);
  return value ? value : "";
}

inline bool
requested()
{
  const auto branch = env("GIT_BRANCH");
  return branch == "codex/indexer-rocks-phases" ||
         branch == "codex/indexer-sorted-probe";
}

inline bool
enabled(std::string_view host)
{
  if (!requested()) {
    return false;
  }
  const auto xid = env("HOG_XID");
  if (env("HOGD_CI_CONTAINER") != "1" || env("HOGD_CI_OFFLINE") != "1" ||
      env("HOG_HOST") != host || env("GIT_COMMIT") != version::kHogdGitHash ||
      !std::regex_match(xid.begin(), xid.end(),
                        std::regex("hogd-multiexec-[0-9]+-[0-9]+"))) {
    panic("indexer phase diagnostic requires its exact offline MultiExec role");
  }
  return true;
}

inline void
open(std::ofstream &output, std::string_view filename)
{
  const auto directory = std::filesystem::path(env("HOG_DUMP_DIR"));
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory)) {
    panic("indexer phase diagnostic requires an absolute existing dump directory");
  }
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output.open(directory / filename, std::ios::out | std::ios::noreplace);
  output << "indexer_phase_v2\t" << version::kHogdGitHash << '\t'
         << env("GIT_BRANCH") << '\t' << env("HOG_XID") << '\t'
         << env("HOG_HOST") << '\n' << std::flush;
}

} // namespace hogd::indexer_phase
