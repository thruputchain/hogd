add_library(
    loadgen
    payment_experiment.cc
)

target_link_libraries(
    loadgen
    hogd
    protocol
)
