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
