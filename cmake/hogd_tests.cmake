# gtest_discover_tests enumerates a test binary at build time by running it with
# --gtest_list_tests, under a timeout that CMake defaults to 5 seconds. Every
# hogd test binary pays real startup before printing a single test name: init
# raises RLIMIT_NOFILE, initializes crypto and logs a version banner. On a
# loaded CI runner that 5 second budget is enough to kill the enumeration
# mid-listing, which fails the build in whichever target happened to be slow
# rather than in anything the change under test touched.
#
# Route discovery through one wrapper so the budget lives in a single place.
set(HOGD_TEST_DISCOVERY_TIMEOUT 60 CACHE STRING
  "Seconds gtest_discover_tests may spend enumerating a test binary")

function(hogd_discover_tests target)
  # DISCOVERY_TIMEOUT has to precede ${ARGN}: gtest_discover_tests parses
  # PROPERTIES as a multi-value keyword, so anything after it is swallowed as a
  # test property instead of being read as an option.
  gtest_discover_tests(${target}
    DISCOVERY_TIMEOUT ${HOGD_TEST_DISCOVERY_TIMEOUT}
    ${ARGN})
endfunction()
