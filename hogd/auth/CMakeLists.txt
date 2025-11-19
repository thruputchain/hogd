
# Tests
add_executable(auth_test
    auth_data_cache_test.cc
)

target_link_libraries(auth_test
    hogd
    GTest::gtest
)
gtest_discover_tests(auth_test)

