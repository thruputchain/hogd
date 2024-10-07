
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
        ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/status.cc
    DEPENDS 
        ${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml 
        ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
        ${CMAKE_SOURCE_DIR}/hogd/hogexec/runtime/status.cc_template
    COMMAND python3 ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py 
    --output_dir=${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/
    --syscall_yaml=${CMAKE_CURRENT_SOURCE_DIR}/syscalls.yaml
    --template_file=${CMAKE_SOURCE_DIR}/hogd/hogexec/runtime/status.cc_template
    --status_cc
    COMMENT
    "generating status.cc"
)

add_custom_target(
    generate_status_cc ALL
    DEPENDS ${CMAKE_BINARY_DIR}/hogd/hogexec/runtime/status.cc
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

