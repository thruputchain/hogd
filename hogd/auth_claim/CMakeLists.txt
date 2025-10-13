add_library(auth_claim_objects OBJECT
    status.cc
    verify.cc
)

add_library(auth_claim STATIC $<TARGET_OBJECTS:auth_claim_objects>)

target_link_libraries(
    auth_claim_objects
    hogd_interface
)

target_link_libraries(
    auth_claim
    hogd
)

add_executable(
    auth_claim_tests 
    auth_claim_tests.cc
)

target_link_libraries(
    auth_claim_tests
    auth_claim
    hogd
    GTest::gtest
)
gtest_discover_tests(auth_claim_tests)
