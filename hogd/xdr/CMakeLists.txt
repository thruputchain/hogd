add_xdr(
	protocol
	../../protocol/contract.x
	../../protocol/coord.x
	../../protocol/executor_to_db_rpc.x
	../../protocol/ffi_enums.x
	../../protocol/rpc_types.x
	../../protocol/transaction.x
	../../protocol/types.x 
	../../protocol/storage_item.x
	syscall.x 
	syscall_args.x
)
add_dependencies(build_protocol generate_syscall_x generate_syscall_args_x)

add_xdr_rs(
	ffi_enum_rs 
	${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src
	../../protocol/ffi_enums.x)

