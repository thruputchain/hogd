add_xdr(
	protocol
	../../protocol/contract.x
	coord.x
	../../protocol/db_read_interface.x
	ffi_enums.x
	../../protocol/rpc_types.x
	../../protocol/transaction.x
	../../protocol/types.x 
	../../protocol/storage_item.x
	syscall.x 
	syscall_args.x
)
add_dependencies(build_protocol generate_syscall_x generate_syscall_args_x)

add_xdr_rs(
	protocol_rs
	${CMAKE_CURRENT_SOURCE_DIR}/rs_xdr/src
	../../protocol/types.x
	syscall.x
	syscall_args.x)
add_dependencies(build_protocol_rs generate_syscall_x generate_syscall_args_x)

add_xdr_rs(
	ffi_enum_rs 
	${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src
	ffi_enums.x)

