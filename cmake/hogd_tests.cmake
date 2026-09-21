# gtest_discover_tests enumerates each test binary at build time under a timeout
# CMake defaults to 5 seconds, which a loaded runner can exceed once hogd's own
# startup is counted, failing the build in an unrelated target (see #4102).
# Route discovery through this wrapper so the budget lives in one place.
set(HOGD_TEST_DISCOVERY_TIMEOUT 60 CACHE STRING
  "Seconds gtest_discover_tests may spend enumerating a test binary")

function(hogd_discover_tests target)
  # DISCOVERY_TIMEOUT must precede ${ARGN}: PROPERTIES swallows later args.
  gtest_discover_tests(${target}
    DISCOVERY_TIMEOUT ${HOGD_TEST_DISCOVERY_TIMEOUT}
    ${ARGN})
endfunction()
