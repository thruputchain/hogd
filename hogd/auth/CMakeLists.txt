add_library(auth
    auth_server.cc
    dbauth_client.cc
    status.cc
)

target_link_libraries(auth
    hogd
    new_metrics 
    auth_claim
)

add_executable(auth_test
    auth_data_cache_test.cc
    auth_server_test.cc
    auth_verify_test.cc
    mock_dbauth_rpc.cc
)

target_link_libraries(auth_test
    auth
    GTest::gtest
)
gtest_discover_tests(auth_test)

add_executable(
  auth_exec
  auth.cc
)
target_link_libraries(
  auth_exec
  auth
  new_metrics
  http
)
set_property(TARGET auth_exec PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
