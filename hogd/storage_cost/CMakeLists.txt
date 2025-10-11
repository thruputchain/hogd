add_library(
  storage_cost
  storage_cost_model.cc
)

target_link_libraries(
  storage_cost
  hogd
)

add_dependencies(
  storage_cost
  hogd
)
