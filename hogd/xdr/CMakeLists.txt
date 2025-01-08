add_xdr(
	protocol 
	types.x 
	syscall.x 
	contract.x
	storage_object.x
	ffi_enums.x
	transaction.x)

add_dependencies(build_protocol generate_syscall_x)

add_xdr_rs(
	protocol_rs 
	${CMAKE_CURRENT_SOURCE_DIR}/rs_xdr/src 
	types.x 
	syscall.x)

add_dependencies(build_protocol_rs generate_syscall_x)

add_xdr_rs(
	ffi_enum_rs 
	${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src
	ffi_enums.x)
