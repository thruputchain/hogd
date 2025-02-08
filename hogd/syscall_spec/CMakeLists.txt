
add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/xdr/syscall.x
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py 
    --output_dir=${CMAKE_BINARY_DIR}/hogd/xdr/ 
    --syscall_yaml=${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml
    --syscall_x
)

add_custom_target(
	generate_syscall_x ALL
	DEPENDS ${CMAKE_BINARY_DIR}/hogd/xdr/syscall.x
)

add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/xdr/syscall_args.x
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py
    --output_dir=${CMAKE_BINARY_DIR}/hogd/xdr/
    --syscall_yaml=${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml
    --syscall_args_x
)

add_custom_target(
	generate_syscall_args_x ALL
	DEPENDS ${CMAKE_BINARY_DIR}/hogd/xdr/syscall_args.x
)

add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/syscall_inner.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py 
    --output_dir=${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/
    --syscall_yaml=${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml
    --syscall_inner_h
)

add_custom_target(
    generate_syscall_inner_h ALL
    DEPENDS ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/syscall_inner.h
)

add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/syscall_entry.cc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py 
    --output_dir=${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/
    --syscall_yaml=${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml
    --syscall_entry_cc
    COMMENT
    "generating syscall_entry.cc"
)

add_custom_target(
    generate_syscall_entry_cc ALL
    DEPENDS ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/syscall_entry.cc
)

add_custom_command(
    OUTPUT
        ${CMAKE_SOURCE_DIR}/hogd/rs_contract_sdk/src/syscall_bindings.rs
    DEPENDS 
        ${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml 
        ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND python3 ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py 
    --output_dir=${CMAKE_SOURCE_DIR}/hogd/rs_contract_sdk/src/
    --syscall_yaml=${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml
    --syscall_rs
    COMMENT
    "generating syscall bindings rs"
)

add_custom_target(
    generate_syscall_rs ALL
    DEPENDS ${CMAKE_SOURCE_DIR}/hogd/rs_contract_sdk/src/syscall_bindings.rs
)

