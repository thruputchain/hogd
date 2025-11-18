add_library(indexer STATIC
    db.cc
    key_format.cc
    status.cc
)

target_link_libraries(indexer PUBLIC
    rocksdb
    hogd
)

# Tests
add_executable(indexer_test
    db_test.cc
    key_format_test.cc
)

target_link_libraries(indexer_test
    indexer
    gtest
)

add_test(NAME indexer_test COMMAND indexer_test)

# Benchmark executable
add_executable(indexer_bench
    indexer_bench.cc
)

target_link_libraries(indexer_bench
    indexer
    hogd
)

set_property(TARGET indexer_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION True)
