file(WRITE "${CMAKE_BINARY_DIR}/test_p2564r3.cc"
  "static_assert(__cpp_consteval >= 202211L,
              \"old consteval support doesn't propagate upwards\");
consteval int f(int i) {
  return i;
}
constexpr int g(int i) {
  if consteval {
    return f(i);
  } else {
    return i;
  }
}
int main() {
  return g(1);
}
")

try_compile(HAS_CONSTEVAL_SUPPORT
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_BINARY_DIR}/test_p2564r3.cc"
)

# If the compilation fails, set a variable for BROKEN_P2564R3
if(NOT HAS_CONSTEVAL_SUPPORT)
    message(STATUS "P2564R3 feature is broken.")
    set(HAVE_BROKEN_P2564R3 1 CACHE INTERNAL "Broken P2564R3 detected")
else()
    message(STATUS "P2564R3 is properly implemented.")
endif()

file(REMOVE "${CMAKE_BINARY_DIR}/test_p2564r3.cc")
