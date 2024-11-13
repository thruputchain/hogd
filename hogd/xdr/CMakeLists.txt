add_xdr(
	protocol 
	types.x 
	syscall.x 
	contract.x)

add_dependencies(protocol generate_syscall_x)

add_xdr_rs(
	protocol_rs 
	${CMAKE_CURRENT_SOURCE_DIR}/rs_xdr/src 
	types.x 
	syscall.x)

add_dependencies(protocol_rs generate_syscall_x)

