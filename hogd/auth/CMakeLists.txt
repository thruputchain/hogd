add_library(auth
    auth_server.cc
    dbauth_client.cc
    status.cc
)

target_link_libraries(auth
    hogd
    auth_claim
)

add_executable(auth_test
    auth_data_cache_test.cc
    auth_server_test.cc
    mock_dbauth_rpc.cc
)

target_link_libraries(auth_test
    auth
    GTest::gtest
)
gtest_discover_tests(auth_test)

