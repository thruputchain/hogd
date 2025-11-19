add_library(auth
    dbauth_client.cc
    status.cc
)

target_link_libraries(auth
    hogd
)

add_executable(auth_test
    auth_data_cache_test.cc
)

target_link_libraries(auth_test
    auth
    GTest::gtest
)
gtest_discover_tests(auth_test)

