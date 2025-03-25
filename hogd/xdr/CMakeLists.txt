add_xdr(
	protocol
	contract.x
	coord.x
	types.x 
	storage_object.x
	syscall.x 
	syscall_args.x
)

add_dependencies(protocol generate_syscall_x generate_syscall_args_x)

add_xdr_rs(
	protocol_rs
	${CMAKE_CURRENT_SOURCE_DIR}/rs_xdr/src
	types.x
	syscall.x
	syscall_args.x)

add_dependencies(protocol_rs generate_syscall_x generate_syscall_args_x)

