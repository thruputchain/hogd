add_executable(hackathon_corpus EXCLUDE_FROM_ALL corpus.cc)
target_include_directories(hackathon_corpus PRIVATE
  ${HACKATHON_ARTIFACT_DIR}
  ${CMAKE_BINARY_DIR}
)
target_link_libraries(hackathon_corpus loadgen)
