

add_library(
    preset_contracts
    shared.cc
)

target_link_libraries(
  preset_contracts
  protocol
  hogd
)

add_dependencies(
  preset_contracts
  protocol
)