add_library(hog_auth
    auth_server.cc
    dbauth_client.cc
    status.cc
)

target_link_libraries(hog_auth
    hogd
    metrics 
    auth_claim
)

add_executable(hog_auth_test
    auth_batch_lookup_test.cc
    auth_data_cache_test.cc
    auth_server_test.cc
    auth_verify_test.cc
    mock_dbauth_rpc.cc
)

target_link_libraries(hog_auth_test
    hog_auth
    GTest::gtest
)
hogd_discover_tests(hog_auth_test)

if(BUILD_BENCHMARKS)
  # In-process auth server benchmarks against a mock DB; see
  # scripts/ci/auth_bench.sh for the CI sweep.
  add_executable(hog_auth_bench
      auth_server_bench.cc
      mock_dbauth_rpc.cc
  )
  target_link_libraries(hog_auth_bench
      hog_auth
      benchmark::benchmark
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET hog_auth_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

add_executable(
  hog-auth
  hog_auth.cc
)
target_link_libraries(
  hog-auth
  hog_auth
  metrics
  http
)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set_property(TARGET hog-auth PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
