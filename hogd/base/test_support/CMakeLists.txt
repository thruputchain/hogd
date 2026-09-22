# Allocation fault injection for tests. Replaces the global allocation
# functions, so link it into a test binary at most once, via
# $<TARGET_OBJECTS:alloc_fault_test_support>.
add_library(alloc_fault_test_support OBJECT
  alloc_fault.cc
)
target_link_libraries(
  alloc_fault_test_support
  hogd_interface
)
