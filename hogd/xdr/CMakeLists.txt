add_xdr(
	protocol
	../../protocol/account.x
	../../protocol/batch.x
	../../protocol/catchup.x
	../../protocol/ci_server.x
	../../protocol/contract.x
	../../protocol/contract_error.x
	../../protocol/coord.x
	../../protocol/coord_sched.x
	../../protocol/db_coord.x
	../../protocol/db_sched.x
	../../protocol/discovery.x
	../../protocol/event.x
	../../protocol/event_rpc.x
	../../protocol/exec_coord.x
	../../protocol/executor_to_db_rpc.x
	../../protocol/ffi_enums.x
	../../protocol/genesis.x
	../../protocol/loadgen.x
	../../protocol/nomination.x
	../../protocol/precompiles.x
	../../protocol/rpc_types.x
	../../protocol/sc_types.x
	../../protocol/sc_val.x
	../../protocol/sched_coord.x
	../../protocol/storage_cost.x
	../../protocol/storage_delta_flags.x
	../../protocol/storage_item.x
	../../protocol/storage_lock.x
	../../protocol/transaction.x
	../../protocol/types.x
	../../protocol/retrocausal_queue.x
	../../protocol/syscall_error.x
	syscall.x
	syscall_args.x
)
target_include_directories(protocol INTERFACE "${CMAKE_SOURCE_DIR}")
target_include_directories(protocol INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(build_protocol generate_syscall_x generate_syscall_args_x)
target_link_libraries(protocol INTERFACE crypto_interface)

add_xdr_rs(
	ffi_enum_rs 
	${CMAKE_SOURCE_DIR}/hogd/rust/wasmi/src
	../../protocol/ffi_enums.x)
