include(CheckCXXSourceCompiles)

check_cxx_source_compiles(
  [=[int main() {
    __builtin_ia32_pause();
    return 0;
}
]=] HAVE_BUILTIN_IA32_PAUSE)

check_cxx_source_compiles(
  [=[#include <arm_acle.h>
int main() {
    __builtin_arm_yield();
    return 0;
}
]=] HAVE_BUILTIN_ARM_YIELD)
