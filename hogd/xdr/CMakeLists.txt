add_xdr(
	protocol 
	contract.x
	coord.x
	ffi_enums.x
	rpc.x
	rpc_types.x
	storage_item.x
	storage_lock.x
	syscall.x
	transaction.x
	types.x)

add_dependencies(build_protocol generate_syscall_x)

add_xdr_rs(
	protocol_rs
	${CMAKE_SOURCE_DIR}/rust_contracts/groundhog_xdr/src
	ffi_enums.x
	types.x
	transaction.x
	contract.x
	syscall.x)

add_dependencies(build_protocol_rs generate_syscall_x)

add_xdr_rs(
	ffi_enum_rs 
	${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src
	ffi_enums.x)
