
if(DEFINED ENV{HOGD_PYTHON})
    set(HOGD_PYTHON "$ENV{HOGD_PYTHON}")
else()
    find_program(HOGD_PYTHON python3 REQUIRED)
endif()

add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/xdr/syscall.x
    DEPENDS ${CMAKE_SOURCE_DIR}/protocol/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND ${HOGD_PYTHON} ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py
    --output_dir=${CMAKE_BINARY_DIR}/hogd/xdr/ 
    --syscall_yaml=${CMAKE_SOURCE_DIR}/protocol/syscalls.yaml
    --syscall_x
)

add_custom_target(
	generate_syscall_x ALL
	DEPENDS ${CMAKE_BINARY_DIR}/hogd/xdr/syscall.x
)

add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/hog_exec/runtime/syscall_inner.h
    DEPENDS ${CMAKE_SOURCE_DIR}/protocol/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND ${HOGD_PYTHON} ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py
    --output_dir=${CMAKE_BINARY_DIR}/hogd/hog_exec/runtime/
    --syscall_yaml=${CMAKE_SOURCE_DIR}/protocol/syscalls.yaml
    --syscall_inner_h
)

add_custom_target(
    generate_syscall_inner_h ALL
    DEPENDS ${CMAKE_BINARY_DIR}/hogd/hog_exec/runtime/syscall_inner.h
)

add_custom_command(
    OUTPUT
        ${CMAKE_BINARY_DIR}/hogd/hog_exec/runtime/syscall_meta.h
    DEPENDS ${CMAKE_SOURCE_DIR}/protocol/syscalls.yaml ${CMAKE_CURRENT_SOURCE_DIR}/gen_syscalls.py
    COMMAND ${HOGD_PYTHON} ${CMAKE_SOURCE_DIR}/hogd/syscall_spec/gen_syscalls.py
    --output_dir=${CMAKE_BINARY_DIR}/hogd/hog_exec/runtime/
    --syscall_yaml=${CMAKE_SOURCE_DIR}/protocol/syscalls.yaml
    --syscall_meta_h
)

add_custom_target(
    generate_syscall_meta_h ALL
    DEPENDS ${CMAKE_BINARY_DIR}/hogd/hog_exec/runtime/syscall_meta.h
)
