// -*-C++-*-

#pragma once

#include <string>
#include <string_view>
#include <typeinfo>

#include <x23/config.h>

#if HAVE_CXA_DEMANGLE

#include <cxxabi.h>
#include <stdlib.h>

// Return a human-readable name for a type.  Attempts to demangle the
// C++ type name if the the abi library is available.
namespace x23 {
template<typename T>
std::string_view
type_name()
{
  static const std::string ret = [] -> std::string {
    const char *mangled = typeid(T).name();
    int status;
    char *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status)
      return mangled;
    std::string ret(demangled);
    free(demangled);
    return ret;
  }();
  return ret;
}
} // namespace x23

#else // !HAVE_CXA_DEMANGLE

namespace x23 {
template<typename T>
const char *
type_name()
{
  return typeid(T).name();
}
} // namespace x23

#endif // !HAVE_CXA_DEMANGLE
