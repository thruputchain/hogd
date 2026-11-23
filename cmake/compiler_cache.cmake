# GCC can resolve system-header symlinks when the absolute path is shorter.
# Preserve the launcher's relative aliases in dependencies and __FILE__ so a
# short CI checkout shares cache entries with a longer developer checkout.
# Select by CMake's compiler ID: Clang does not accept this GCC option.
add_compile_options(
  "$<$<COMPILE_LANG_AND_ID:C,GNU>:-fno-canonical-system-headers>"
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fno-canonical-system-headers>"
)
