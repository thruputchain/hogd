add_xdr(
	protocol
	../../protocol/batch.x
	../../protocol/catchup.x
	../../protocol/contract.x
	../../protocol/coord.x
	../../protocol/db_coord.x
	../../protocol/discovery.x
	../../protocol/exec_coord.x
	../../protocol/executor_to_db_rpc.x
	../../protocol/ffi_enums.x
	../../protocol/nomination.x
	../../protocol/rpc_types.x
	../../protocol/sched_coord.x
	../../protocol/storage_item.x
	../../protocol/storage_lock.x
	../../protocol/transaction.x
	../../protocol/types.x
	../../protocol/retrocausal_queue.x
	syscall.x 
	syscall_args.x
)
add_dependencies(build_protocol generate_syscall_x generate_syscall_args_x)

add_xdr_rs(
	ffi_enum_rs 
	${CMAKE_SOURCE_DIR}/hogd/hogexec/wasmi/src
	../../protocol/ffi_enums.x)
