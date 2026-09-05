add_library(hog_indexer STATIC
    db.cc
    event_cursor.cc
    key_format.cc
    pull_controller.cc
    status.cc
    topic_layout.cc
)

target_link_libraries(hog_indexer PUBLIC
    rocksdb
    indexer_payload_validation
    metrics
    hogd
)

# Tests
add_executable(hog_indexer_test
    indexer_test.cc
    indexer_process_test.cc
    recent_tx_failure_cache_test.cc
    db_test.cc
    event_cursor_test.cc
    key_format_test.cc
    pull_controller_test.cc
    strict_ingest_test.cc
    topic_layout_test.cc
)

target_compile_definitions(
  hog_indexer_test
  PRIVATE
  HOG_INDEXER_BINARY="$<TARGET_FILE:hog-indexer>"
)
target_link_libraries(hog_indexer_test
    hog_indexer
    hog_event
    gtest
)
gtest_discover_tests(hog_indexer_test)

if(BUILD_BENCHMARKS)
    # Benchmark executable
    add_executable(hog_indexer_bench
        indexer_bench.cc
    )

    target_link_libraries(hog_indexer_bench
        hog_indexer
        hog_event
        hogd
    )

    set_property(TARGET hog_indexer_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION True)
endif()

add_executable(
  hog-indexer
  hog_indexer.cc
)
target_link_libraries(
  hog-indexer
  hog_indexer
  metrics
  http
)
add_dependencies(hog_indexer_test hog-indexer)
