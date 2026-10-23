if(BUILD_BENCHMARKS)
  add_executable(
    fibonacci_rv32only
    fibonacci_rv32only.cc
  )
  target_link_libraries(
    fibonacci_rv32only
    hogd
    protocol
    runtime
    exec_interface
    hog_db
    runtime_test_util #TODO(geoff): for the local event server; remove this once we have a proper event server
    contract_loader
  )

  target_include_directories(
    fibonacci_rv32only
    PRIVATE
    ${CMAKE_BINARY_DIR}
  )

  add_dependencies(
    fibonacci_rv32only
    all_riscvs_built # for fibonacci.rv32a
    all_contract_headers # for built_contracts/fibonacci.h
  )

  set_property(TARGET fibonacci_rv32only PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

  add_executable(
    payments_bench
    payments_bench.cc
  )
  target_link_libraries(
    payments_bench
    hogd
    protocol
    runtime
    exec_interface
    hog_db
    runtime_test_util #TODO(geoff): for the local event server; remove this once we have a proper event server
    contract_loader
    loadgen
  )

  target_include_directories(
    payments_bench
    PRIVATE
    ${CMAKE_BINARY_DIR}
  )

  add_dependencies(
    payments_bench
    all_riscvs_built # for itc_erc20.rv32a / simple_wallet.rv32a (payments_bench -r)
    all_contract_headers # for built_contracts/itc_erc20.h / simple_wallet.h
  )

  set_property(TARGET payments_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

  add_executable(
    bench_server
    bench_server.cc
  )
  target_link_libraries(
    bench_server
    hogd
    protocol
    runtime
    hog_db
  )

  set_property(TARGET bench_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_executable(
  control_server
  control_server.cc
)
target_link_libraries(
  control_server
  hogd
  protocol
  runtime
  hog_db
)

set_property(TARGET control_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  control_cli
  control_cli.cc
)
target_link_libraries(
  control_cli
  hogd
  protocol
)

set_property(TARGET control_cli PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

add_executable(
  multiexec_runner
  multiexec_runner.cc
)
target_link_libraries(
  multiexec_runner
  hogd
  protocol
  runtime
  exec_interface
  hog_db
  runtime_test_util #TODO(geoff): for the local event server; remove this once we have a proper event server
  contract_loader
  loadgen
)

target_include_directories(
  multiexec_runner
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

set_property(TARGET multiexec_runner PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# Add out-of-tree benchmark executables, such as Whitney's runner, in the
# same directory scope as multiexec_runner.
set(HOGD_EXTRA_BENCH_DIRS "" CACHE STRING "Semicolon-separated absolute directories containing extra benchmarks")
set(extra_bench_index 0)
foreach(extra_bench_dir IN LISTS HOGD_EXTRA_BENCH_DIRS)
  if(NOT IS_ABSOLUTE "${extra_bench_dir}" OR
     NOT IS_DIRECTORY "${extra_bench_dir}" OR
     NOT EXISTS "${extra_bench_dir}/CMakeLists.txt" OR
     IS_DIRECTORY "${extra_bench_dir}/CMakeLists.txt")
    message(FATAL_ERROR "HOGD_EXTRA_BENCH_DIRS entry must be an absolute path to an existing directory with a CMakeLists.txt: ${extra_bench_dir}")
  endif()
  get_filename_component(extra_bench_name "${extra_bench_dir}" NAME)
  add_subdirectory("${extra_bench_dir}" "${CMAKE_CURRENT_BINARY_DIR}/extra/${extra_bench_index}-${extra_bench_name}")
  math(EXPR extra_bench_index "${extra_bench_index} + 1")
endforeach()

add_executable(
  makespan_runner
  makespan_runner.cc
)
target_link_libraries(
  makespan_runner
  hogd
  protocol
  runtime
  exec_interface
  hog_db
  runtime_test_util #TODO(geoff): for the local event server; remove this once we have a proper event server
  contract_loader
)

target_include_directories(
  makespan_runner
  PRIVATE
  ${CMAKE_BINARY_DIR}
)

add_dependencies(
  makespan_runner
  all_riscvs_built # for fibonacci.rv32a / storage_reads.rv32a
  all_contract_headers # for built_contracts/fibonacci.h / storage_reads.h
)

set_property(TARGET makespan_runner PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
