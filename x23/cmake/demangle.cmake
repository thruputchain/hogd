include(CheckCXXSourceCompiles)

check_cxx_source_compiles([=[
#include <stdlib.h>
#include <cxxabi.h>
#include <typeinfo>
#include <string>

int main() {
  const char* mangled_name = typeid(std::string).name();
  int status = 0;
  char* demangled_name =
      abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
  if (demangled_name) {
    free(demangled_name);
    return 0;
  }
  return 1;
}
]=] HAVE_CXA_DEMANGLE)
