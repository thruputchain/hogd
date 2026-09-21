# gtest_discover_tests runs each test binary at build time to enumerate its
# cases, under a 5s default that a loaded runner can exceed, failing the build
# in an unrelated target (#4102). This wrapper keeps the budget in one place.
set(HOGD_TEST_DISCOVERY_TIMEOUT 60 CACHE STRING
  "Seconds gtest_discover_tests may spend enumerating a test binary")

function(hogd_discover_tests target)
  # DISCOVERY_TIMEOUT must precede ${ARGN}: PROPERTIES swallows later args.
  gtest_discover_tests(${target}
    DISCOVERY_TIMEOUT ${HOGD_TEST_DISCOVERY_TIMEOUT}
    ${ARGN})
endfunction()
