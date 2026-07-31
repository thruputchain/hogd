add_library(indexer STATIC
    db.cc
    event_cursor.cc
    key_format.cc
    registration_controller.cc
    status.cc
    topic_layout.cc
)

target_link_libraries(indexer PUBLIC
    rocksdb
    indexer_payload_validation
    metrics
    hogd
)

# Tests
add_executable(indexer_test
    indexer_test.cc
    recent_tx_failure_cache_test.cc
    db_test.cc
    event_cursor_test.cc
    key_format_test.cc
    registration_controller_test.cc
    strict_ingest_test.cc
    topic_layout_test.cc
)

target_link_libraries(indexer_test
    indexer
    event
    gtest
)
gtest_discover_tests(indexer_test)

if(BUILD_BENCHMARKS)
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
endif()

add_executable(
  indexer_exec
  indexer.cc
)
target_link_libraries(
  indexer_exec
  indexer
  metrics
  http
)
