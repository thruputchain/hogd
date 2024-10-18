#pragma once

#include <cstdint>
#include <map>

#include "hogd/hogexec/runtime/call_frame.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/storage_proxy.h"
#include "hogd/hogexec/runtime/wasmi_runtime.h"
#include "storage_interface.hh"
#include "storage_object.hh"
#include "syscall.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/rpc.h"

namespace hogd {

class StorageSnapshotCache;

class SystemCallHandler {
public:
  SystemCallHandler(SignedTransaction signed_tx,
                    StorageSnapshotCache &snapshot_cache);

  InvokeResult root_invoke();

  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke(Invocation const &invocation);

  // syscall methods: can only be called when call_frame_stack_ is nonempty
  // (i.e. while at least one invoke() is running)
  ContractAddress get_current_address() const;

  SyscallStatus<ContractAddress> get_msg_sender() const;
  SHA256Hash get_source_tx_hash() const;

  // general storage
  void delete_last(StorageKey const &key);
  SyscallStatus<StorageObjectType> get_object_type(StorageKey const &key);

  // bytestring
  SyscallStatus<const Bytestring> get_bytestring(StorageKey const &key);
  SyscallStatus<RuntimeMemoryLength> get_bytestring_len(StorageKey const &key);
  SyscallStatus<void>
  set_bytestring(StorageKey const &key,
                 std::span<const std::byte> input_bytestring);

  // nnint64
  SyscallStatus<const int64_t> get_nnint64_base_value(StorageKey const &key);
  SyscallStatus<const int64_t> get_nnint64_value(StorageKey const &key);
  SyscallStatus<void> nnint64_set_add(StorageKey const &key, int64_t set_value,
                                      int64_t add_value);

  SyscallStatus<Calldata> get_calldata() const;
  SyscallStatus<RuntimeMemoryLength> get_calldata_len() const;

  SyscallStatus<const ReturnBuffer> get_return_buffer() const;
  void set_return_buffer(ReturnBuffer &&returned_buffer);

  SyscallStatus<void> log(std::span<const std::byte> message);

  std::vector<LogEntry> const &get_logs() const { return committed_logs_; }
  StorageDeltaVecList const &get_deltas() const { return committed_delta_vec_; }

  RuntimeMemoryLength get_defaultzero_return_len() const;

  static ContractAddress
  static_get_deploy_address(ContractAddress const &sender,
                            SHA256Hash const &nonce,
                            SHA256Hash const &contract_hash);

  ContractAddress get_deploy_address(SHA256Hash const &nonce,
                                     SHA256Hash const &contract_hash) const;

  SyscallStatus<ContractAddress>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  using write_client_t = StorageWriteInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  StorageRPCError try_reserve_transaction(write_client_t &client) const;
  void commit_transaction(write_client_t &client) const;

private:
  struct RuntimeCacheEntry {
    WasmiRuntime runtime;
    bool unwind_poisoned = false;

    RuntimeCacheEntry(WasmiRuntime&& runtime)
      : runtime(std::move(runtime)) {}

    RuntimeCacheEntry(RuntimeCacheEntry &) = delete;
  };

  WasmiContext wasmi_context_;

  RuntimeCacheEntry *resolve_runtime(ContractAddress const &addr);
  InvokeResult invoke_unconverted(Invocation const &invocation,
                                  RuntimeCacheEntry *runtime,
                                  uint64_t gas_limit);

  // Can only be called when call_frame_stack_ is nonempty
  StorageProxy &resolve_storage_proxy(StorageKey const &key);

  SignedTransaction signed_tx_;
  std::vector<CallFrame> call_frame_stack_;

  std::map<ContractAddress, std::unique_ptr<RuntimeCacheEntry>> runtimes_;

  std::map<ContractAddress, SHA256Hash> new_deployments_;
  const std::map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  // only set at the end of the transaction
  std::vector<LogEntry> committed_logs_;
  StorageDeltaVecList committed_delta_vec_;

  StorageSnapshotCache &snapshot_cache_;

  void commit_last_call_frame(CallFrame &&frame);

  void commit_new_contract_deployments(write_client_t &client) const;
};

} // namespace hogd
