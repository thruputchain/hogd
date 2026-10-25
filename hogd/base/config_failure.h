// config_failure.h - report misconfiguration instead of aborting

#pragma once

#include <exception>
#include <optional>
#include <string>

namespace hogd {

// Returns the operator-facing message for an exception that means the process
// was started into a conflicting environment or given an unusable address -- a
// listen port that is already in use, or a peer address that does not resolve
// -- or std::nullopt for any other exception (or a null [e]).
std::optional<std::string> config_failure_message(std::exception_ptr e);

// Installs a std::terminate handler that, when the exception that reached
// std::terminate is one [config_failure_message] recognizes, logs that message
// and exits with EXIT_FAILURE. Anything else falls through to the previously
// installed handler, so genuine bugs (and panic(), which terminates with no
// exception) still abort with a core dump.
//
// This is a terminate handler rather than a catch in each main() because under
// the Itanium C++ ABI (GCC and Clang, on Linux and macOS) an exception that no
// handler catches reaches std::terminate without unwinding the stack. The
// process therefore exits without running the destructors of the half-started
// servers above the throw, which a catch in main() would run; ~Executor, for
// one, waits for a shutdown request that never comes.
void install_config_failure_handler();

} // namespace hogd
