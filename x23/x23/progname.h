// -*- C++ -*-

#include <filesystem>
#include <string_view>

#pragma once

namespace x23 {

// If prog does not contain any '/' characters, then look for it in
// the directories of the PATH environment variable.
std::filesystem::path find_exe(std::filesystem::path prog);

// Set argv0 for debug logging purposes, in case it can't be inferred
void setprogpath(std::string_view argv0);

// Get path to current executable (if available)
const std::string &getprogpath();

// Get name of current executable (if available)
const std::string &getprogname();

} // namespace x23
