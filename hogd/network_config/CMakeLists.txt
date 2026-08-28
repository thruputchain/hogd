add_library(
  network_config
  config_upgrade.cc
)

target_link_libraries(
  network_config
  hogd
)

add_dependencies(
  network_config
  hogd
)

add_executable(
  network_config_test
  config_upgrade_test.cc
)

target_link_libraries(
  network_config_test
  network_config
  hogd
  GTest::gtest_main
)
gtest_discover_tests(network_config_test)
