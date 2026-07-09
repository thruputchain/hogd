# Header-only: the canonical key->shard routing (shard_router.h) and the pure
# cross-shard batch split/gather core (sharded_batch.h) live entirely in
# headers. Consumers just include them (the source root is a global include
# dir), so there is no link dependency to propagate.
add_library(db_shard_routing INTERFACE)
target_link_libraries(db_shard_routing INTERFACE hogd_interface)

add_executable(
  db_shard_routing_test
  shard_router_test.cc
  sharded_batch_test.cc
)
target_link_libraries(
  db_shard_routing_test
  db_shard_routing
  hogd
  GTest::gtest_main
)
gtest_discover_tests(db_shard_routing_test)
