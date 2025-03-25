add_xdr(
	protocol
	contract.x
	coord.x
	types.x 
	storage_object.x
	syscall.x 
)

add_dependencies(build_protocol generate_syscall_x)

add_xdr_rs(
	protocol_rs
	${CMAKE_CURRENT_SOURCE_DIR}/rs_xdr/src
	types.x
	syscall.x)

add_dependencies(build_protocol_rs generate_syscall_x)

