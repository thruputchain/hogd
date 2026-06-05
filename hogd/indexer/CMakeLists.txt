add_library(indexer STATIC
    db.cc
    event_cursor.cc
    key_format.cc
    status.cc
)

target_link_libraries(indexer PUBLIC
    rocksdb
    event
    new_metrics
    hogd
)

# Tests
add_executable(indexer_test
    indexer_test.cc
    db_test.cc
    event_cursor_test.cc
    key_format_test.cc
)

target_link_libraries(indexer_test
    indexer
    gtest
)
gtest_discover_tests(indexer_test)

# Benchmark executable
add_executable(indexer_bench
    indexer_bench.cc
)

target_link_libraries(indexer_bench
    indexer
    event
    hogd
)

set_property(TARGET indexer_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION True)

add_executable(
  indexer_exec
  indexer.cc
)
target_link_libraries(
  indexer_exec
  indexer
  new_metrics
  http
)
