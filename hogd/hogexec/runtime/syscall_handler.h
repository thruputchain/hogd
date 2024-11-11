#pragma once

#include <cstdint>
#include <map>

#include "hogd/hogexec/runtime/call_frame.h"
#include "hogd/hogexec/runtime/item_cache.h"
#include "hogd/hogexec/runtime/status.h"
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

  std::pair<HostFnStatus<SyscallErrorCode>, uint64_t>
  invoke_verify_auth(ContractAddress const &authorizing_address,
                     GasAmount call_specified_gas_limit);

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

  // replay cache
  SyscallStatus<void> replay_cache_insert(StorageKey const &key,
                                          uint64_t nonce);
  SyscallStatus<void> replay_cache_clear(StorageKey const &key,
                                         uint64_t threshold);

  // auth
  void push_auth_context(std::span<const std::byte> auth_context_frame);
  SyscallStatus<void> pop_auth_context();

  SyscallStatus<void>
  require_auth(const ContractAddress &authorizing_address,
               std::optional<std::span<const std::byte>> action);
  bool inside_verify_auth_frame() const;
  SyscallStatus<Witness> get_auth_witness(uint32_t wit_index) const;

  SyscallStatus<AuthEntrySignaturePayload>
  get_auth_payload(const SHA256Hash *optional_input_message);

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

  ContractAddress get_deploy_address(
    const SHA256Hash * optional_caller_address, SHA256Hash const &nonce,
                                     SHA256Hash const &contract_hash) const;

  SyscallStatus<ContractAddress>
  deploy_contract(SHA256Hash const &nonce, SHA256Hash const &contract_hash);

  using write_client_t = StorageWriteInterfaceV1::client<
      x23::RpcStream<x23::RpcLogger>::ClientRef>;

  StorageRPCError try_reserve_transaction(write_client_t &client) const;
  void commit_transaction(write_client_t &client) const;

private:
  struct RuntimeCacheEntry {
    WasmiRuntime runtime_;
    const bool local_deployment_;
    bool revert_poisoned_ = false;

    RuntimeCacheEntry(WasmiRuntime &&runtime, bool local_deployment) : runtime_(std::move(runtime)),
    local_deployment_(local_deployment)
    {}

    RuntimeCacheEntry(RuntimeCacheEntry &) = delete;
  };

  WasmiContext wasmi_context_;

  RuntimeCacheEntry *resolve_runtime(ContractAddress const &addr);

  SyscallStatus<GasAmount>
  get_invoke_gas_limit(GasAmount call_specified_gas_limit);

  void start_call_frame(Invocation const &invocation, uint64_t new_flags);
  void start_verify_auth_frame(Invocation const &invocation);

  void finish_call_frame(InvokeResult const &result);

  // Can only be called when call_frame_stack_ is nonempty
  ItemCache &resolve_item_cache(StorageKey const &key);

  SignedTransaction signed_tx_;

  std::vector<CallFrame> call_frame_stack_;

  // RuntimeCacheEntry cannot be moved while it is actively invoked.  unique_ptr
  // required here.
  std::map<ContractAddress, std::unique_ptr<RuntimeCacheEntry>> runtimes_;

  const std::map<SHA256Hash, std::shared_ptr<const Contract>>
      newly_created_contracts_;

  std::map<ContractAddress, SHA256Hash> new_committed_deployments_;

  AuthManager root_auth_manager_;
  ItemCacheMap root_item_cache_map_;
  GasAmount total_gas_consumption_ = 0;

  // only set at the end of the transaction
  std::vector<LogEntry> committed_logs_;
  StorageDeltaVecList committed_delta_vec_;

  StorageSnapshotCache &snapshot_cache_;

  std::optional<SHA256Hash>
  lookup_locally_deployed_contract(ContractAddress const &addr) const;

  auto &get_active_call_frame(this auto &self)
  {
    assert(!self.call_frame_stack_.empty());
    return self.call_frame_stack_.back();
  }

  void commit_last_call_frame_success(CallFrame &frame);
  void commit_new_contract_deployments(write_client_t &client) const;

};

} // namespace hogd
