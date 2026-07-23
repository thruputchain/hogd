#include "hogd/hogexec/runtime/item/database_proxy.h"

#include <cassert>
#include <concepts>
#include <exception>
#include <format>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "contract.hh"
#include "db_conflict.hh"
#include "executor_to_db_rpc.hh"
#include "ffi_enums.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/db_shard_routing/shard_router.h"
#include "hogd/db_shard_routing/sharded_batch.h"
#include "hogd/debug/key_type_check.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/components/borrowed_contract_batch.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "sched_exec.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/cpumap.h"
#include "x23/rpc_msg.hh"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/vec.h"
#include "x23/x23.h" // x23::union_visit_arm

namespace hogd {

namespace {

struct VoidMarker {};
struct NotFoundIsUnrecoverableError {};

constexpr auto
return_type(auto &&f, auto &&err)
{
  if constexpr (requires { f(std::move(get<1>(err))); }) {
    return f(std::move(get<1>(err)));
  }
  else if constexpr (std::same_as<decltype(f()), void>) {
    return;
  }
  else {
    return f();
  }
}

template<typename T>
concept is_expected_type = requires { typename T::value_type; } &&
                           requires { typename T::error_type; };

template<typename T>
constexpr auto
get_inner_return_type()
{
  if constexpr (is_expected_type<T>) {
    return typename T::value_type{};
  }
  else if constexpr (std::same_as<T, void>) {
    return;
  }
  else {
    return T{};
  }
}

template<auto NullValue>
constexpr auto
to_system_error_code(auto err, auto &&f)
{
  constexpr bool kHasReturnType = requires { f(std::move(get<1>(err))); };
  using return_type = decltype(return_type(f, err));
  using inner_return_type = decltype(get_inner_return_type<return_type>());

  return [&] -> SystemStatus<inner_return_type> {
    RPCError err_code;
    if constexpr (kHasReturnType) {
      err_code = err.error();
    }
    else {
      err_code = err;
    }

    switch (err_code) {
    case RPCError::kRPCNone:
      if constexpr (kHasReturnType) {
        return SystemStatus<inner_return_type>{f(std::move(get<1>(err)))};
      }
      else {
        static_assert(std::is_same_v<decltype(err), RPCError>);
        if constexpr (std::same_as<decltype(f()), void>) {
          return SystemStatus<void>{};
        }
        else {
          return f();
        }
      }
    case RPCError::kRPCStorageWriteFailed:
    case RPCError::kRPCNotFound:
      // can't directly combine these branches; f() isn't a valid expression in
      // the first case
      //
      // NOLINTBEGIN(bugprone-branch-clone)
      if constexpr (std::same_as<decltype(NullValue),
                                 NotFoundIsUnrecoverableError>) {
        error("Unrecoverable: Received RPCNotFound or RPCStorageWriteFailed on "
              "unexpected rpc: {}",
              err_code);
        return SystemError{SystemErrorCode::Unrecoverable};
      }
      else if constexpr (kHasReturnType) {
        return SystemStatus<return_type>{NullValue};
      }
      else if constexpr (!std::same_as<decltype(f()), void>) {
        return SystemStatus<return_type>{NullValue};
      }
      // NOLINTEND(bugprone-branch-clone)
      else {
        // This should never happen -- none of the rpc errors that have a return
        // type of void should return this kind of error code.
        assert(false);
        error("Unrecoverable: Received RPCNotFound or RPCStorageWriteFailed on "
              "unexpected rpc: {}",
              err_code);
        return SystemError{SystemErrorCode::Unrecoverable};
      }
    case RPCError::kRPCBadBlockID:
      return SystemError{SystemErrorCode::Abandon};
    default:
      error("Unrecoverable: Received unhandled error type from RPC: {}",
            err_code);
      return SystemError{SystemErrorCode::Unrecoverable};
    }
  }();
}

} // namespace

size_t
DatabaseProxy::look_up_shard(AddressAndKey const &key) const
{
  // Canonical key->shard routing lives in the shared shard-routing module.
  return shard_of_key(key, clients_.size());
}

DatabaseProxy::client_t &
DatabaseProxy::client_for(AddressAndKey const &key)
{
  return clients_[look_up_shard(key)];
}

DatabaseProxy::client_t &
DatabaseProxy::client_for()
{
  // Primary shard: global/keyless requests (db tip, viewing snapshots) and
  // content-addressed lookups (contracts) route here for now.
  return clients_[kPrimaryShard];
}

DatabaseProxy::DatabaseProxy(DatabaseIdentifier db_identifier)
{
  if (std::holds_alternative<LocalMockDB>(db_identifier)) {
    auto mock_db = std::get<LocalMockDB>(db_identifier);
    // A single client for local mock testing. Using one client avoids fd ulimit
    // issues in sdk tests.
    clients_.push_back(make_srpc_multiclient<DatabaseToExecutorInterfaceV1>(
        x23::HostPort{"localhost", x23::uformat("{}", mock_db.read_port_number_)},
        false, *x23::single_bucket_map));
    return;
  }
  if (std::holds_alternative<ShardedDB>(db_identifier)) {
    auto sharded_db = std::get<ShardedDB>(db_identifier);
    assert(!sharded_db.shards_.empty());
    // Connect to every discovered shard; requests are routed per key by
    // look_up_shard / client_for.
    clients_.reserve(sharded_db.shards_.size());
    for (ShardCard const &shard : sharded_db.shards_) {
      x23::HostPort hostport;
      if (!hostport.parse(shard.address_for_exec)) {
        throw std::runtime_error("Failed to parse database shard address");
      }
      clients_.push_back(
          make_srpc_multiclient<DatabaseToExecutorInterfaceV1>(hostport, false));
    }
    return;
  }
  throw std::runtime_error("unimplemented");
}

SystemStatus<std::optional<WireStorageItem>>
DatabaseProxy::get_snapshot_item(BlockID block_id, AddressAndKey const &addrkey)
{
  auto &client = client_for(addrkey);
  auto res = client.get_snapshot_item(block_id, addrkey);
  return to_system_error_code<std::nullopt>(
      res, [](WireStorageItem &&x) -> std::optional<WireStorageItem> {
        return std::make_optional<WireStorageItem>(std::move(x));
      });
}

SystemStatus<std::optional<Contract>>
DatabaseProxy::get_contract(BlockID block_id, SHA256Hash const &contract_hash)
{
  auto &client = client_for();
  auto res = client.get_contract(block_id, contract_hash);
  return to_system_error_code<std::nullopt>(res, [](Contract &&x) {
    return std::make_optional<Contract>(std::move(x));
  });
}

SystemStatus<std::optional<DeployedContractHash>>
DatabaseProxy::get_deployment(BlockID block_id, ContractAddress const &addr)
{
  auto &client = client_for();
  auto res = client.get_deployment(block_id, addr);
  return to_system_error_code<std::nullopt>(res, [](DeployedContractHash &&x) {
    return std::make_optional<DeployedContractHash>(std::move(x));
  });
}

SystemStatus<std::optional<RetrocausalQueueBlock>>
DatabaseProxy::get_next_retrocausal_queue_block(BlockID block_id,
                                                AddressAndKey const &addrkey,
                                                uint64_t search_start_number)
{
  auto &client = client_for(addrkey);
  auto res = client.get_next_retrocausal_queue_block(block_id, addrkey,
                                                     search_start_number);
  return to_system_error_code<std::nullopt>(res, [](RetrocausalQueueBlock &&x) {
    return std::make_optional<RetrocausalQueueBlock>(std::move(x));
  });
}

SystemStatus<std::optional<QueueEntry>>
DatabaseProxy::get_concurrent_retrocausal_queue_entry(
    BlockID block_id, AddressAndKey const &addrkey, uint32_t index)
{
  auto &client = client_for(addrkey);
  auto res =
      client.get_concurrent_retrocausal_queue_entry(block_id, addrkey, index);
  return to_system_error_code<std::nullopt>(res, [](QueueEntry &&x) {
    return std::make_optional<QueueEntry>(std::move(x));
  });
}

SystemStatus<void>
DatabaseProxy::commit_transaction(BlockID block_id, TransactionID tx_id,
                                  BorrowedContractBatch const &new_contracts)
{
  try {
    // Fan out commit to every shard (each has the tx open from the prepare
    // push) -- the commit phase of the cross-shard 2PC. New contracts are
    // content-addressed and live on the primary shard (shard 0); other shards
    // commit only their own deltas, with an empty contract batch.
    static const BorrowedContractBatch::hash_set_t kEmptyHashes;
    static const BorrowedContractBatch::contract_map_t kEmptyContracts;
    static const BorrowedContractBatch kEmptyBatch(kEmptyHashes,
                                                   kEmptyContracts);
    for (size_t shard = 0; shard < clients_.size(); ++shard) {
      auto res = clients_[shard].commit_transaction(
          block_id, tx_id, shard == 0 ? new_contracts : kEmptyBatch);
      auto status = to_system_error_code<VoidMarker{}>(res, [] {});
      if (!status.has_value()) {
        return status;
      }
    }
    return SystemStatus<void>{};
  } catch (std::exception const &e) {
    // commit_transaction runs on a detached executor worker; any escaping
    // exception terminates hogexec. Contain every failure here and return a
    // structured Unrecoverable result instead. This covers:
    //   - x23::XdrBadInput   -- client-side, oversized new contract batch
    //   - x23::XdrCallFailed -- server-side SYSTEM_ERR (e.g. HogDB's contract
    //                           rehash throwing while re-serializing a
    //                           Contract)
    //   - std::bad_alloc     -- local OOM, e.g. while serializing the request
    // The diagnostic itself allocates while formatting and can throw again
    // under the same memory pressure, so guard it and fall back to the
    // no-allocation reporter before returning (cf. syscall_invoke.cc, #2719).
    try {
      error("Unrecoverable: commit_transaction failed "
            "(transport/serialization/allocation failure): {}",
            e.what());
    } catch (...) {
      report_dropped_log("commit_transaction failure diagnostic");
    }
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<void>
DatabaseProxy::abort_transaction(BlockID block_id, TransactionID tx_id)
{
  // Fan out abort to every shard the transaction has open. Visit ALL shards
  // even after one fails and return the first error: try_push_new_subtransaction
  // may have opened a transaction on shards past a failing one, and stopping
  // early would leak that open state indefinitely (mirrors broadcast_status).
  SystemStatus<void> first_error{};
  for (size_t shard = 0; shard < clients_.size(); ++shard) {
    auto res = clients_[shard].abort_transaction(block_id, tx_id);
    auto status = to_system_error_code<VoidMarker{}>(res, [] {});
    if (!status.has_value() && first_error.has_value()) {
      first_error = status;
    }
  }
  return first_error;
}

// A transaction's r/w footprint must never encode to a StorageDeltaBatch push
// that overflows the executor->HogDB RPC -- that surfaces as a node-
// unrecoverable error. try_push_new_subtransaction below sums a conservative
// per-delta footprint over the batch and rejects an over-cap push as a
// concurrency-control conflict before it is sent, covering every way a push can
// grow: many keys, many/large bytestrings, and RCQ observations. The footprint
// is computed from delta field LENGTHS, never the serializer, so the gate is
// not coupled to ser/des; x23::xdr_size is used only in a debug assert
// (stripped in release) to prove each bound is a true upper bound and catch
// layout drift.
namespace {

// Maximum XDR-encoded size of a type, derived from its layout rather than from
// hand-written constants, so a protocol field/type change is tracked
// automatically. kXdrWord is one RFC 4506 wire word: the length prefix before
// every variable-length array/opaque and the discriminant before every union.
template<typename T>
constexpr size_t kXdrMax = x23::detail::xdr_size_bounds<T>().max_;
constexpr size_t kXdrWord = 4;

// footprint_bound(value) -> a conservative upper bound, in bytes, on value's
// XDR-encoded size. Every overload must (a) return >= the value's actual
// encoded size for every possible value (the debug assert in the push loop
// checks this against x23::xdr_size) and (b) compute that from field lengths
// only -- never serialize, never read the database. This generic overload
// returns the type's compile-time maximum and static_asserts the type is
// fixed-size; a variable-size type must add its own overload that bounds from
// the value's contents (see Bytestring and RetrocausalQueueDelta), else it
// would charge a maximum far above a typical value and turn away transactions
// that would fit.
template<typename T>
constexpr size_t
footprint_bound(T const & /*value*/)
{
  static_assert(x23::detail::xdr_size_bounds<T>().min_ ==
                    x23::detail::xdr_size_bounds<T>().max_,
                "variable-size type needs a content-derived footprint_bound "
                "overload (see Bytestring, RetrocausalQueueDelta)");
  return kXdrMax<T>;
}

// XDR opaque<>: a 4-byte length prefix plus content padded to a 4-byte
// boundary.
constexpr size_t
opaque_bound(size_t len)
{
  return kXdrWord + ((len + 3) & ~size_t{3});
}
static_assert(opaque_bound(Bytestring::kMaxSize) == kXdrMax<Bytestring>);

inline size_t
footprint_bound(Bytestring const &b)
{
  return opaque_bound(b.size());
}

// Auth changes carry a variable signer list but are rare; charging the maximum
// (kMaxSigners signers) flat is acceptable. Explicit so it does not trip the
// fixed-size static_assert in the generic overload above.
inline size_t
footprint_bound(AuthDataChangeArgs const & /*auth*/)
{
  return kXdrMax<AuthDataChangeArgs>;
}

// RCQ delta: the encoded size is dominated by the entry payloads, which vary
// with the delta, so it is bounded from the entry counts (each entry charged
// kXdrMax<QueueEntry>) rather than the type maximum -- the latter is the full
// per-tx observation cap, far above a typical delta. The overhead term covers
// the past-consume struct and the array/optional framing words.
constexpr size_t kRcqDeltaOverhead =
    kXdrMax<RetrocausalQueuePastBlockConsumeDelta> + (4 * kXdrWord);

inline size_t
footprint_bound(RetrocausalQueueDelta const &rq)
{
  size_t entries = rq.insert.added_entries.size() +
                   rq.observed_concurrent_entries.size() +
                   (rq.consume ? rq.consume->current_block_consumed.size() : 0);
  return kRcqDeltaOverhead + (entries * kXdrMax<QueueEntry>);
}

// Dispatch to the active arm's footprint_bound. The delete arm is void (nothing
// to serialize); every other arm carries a value the visitor forwards. The
// fallback is unreachable -- a StorageDelta always has a set discriminant --
// but charges the largest any arm could be, which stays a true upper bound.
inline size_t
footprint_bound(StorageDelta const &d)
{
  if (d.type() == StorageDeltaType::kDeltaTypeDelete)
    return 0;
  return x23::union_visit_arm(
      [](auto const &arm) { return footprint_bound(arm); }, d,
      kXdrMax<StorageDelta>);
}

// Per-FlaggedStorageDelta framing: addrkey + flags + the union discriminant --
// all type-derived.
constexpr size_t kPerDeltaFraming = kXdrMax<AddressAndKey> +
                                    kXdrMax<StorageDeltaFlagSet> +
                                    kXdrMax<StorageDeltaType>;

inline size_t
footprint_bound(FlaggedStorageDelta const &fd)
{
  return kPerDeltaFraming + footprint_bound(fd.delta);
}

// The executor->HogDB RPC message cap a push must fit. x23 bounds
// (record.size() - 4) -- the rpc_msg header plus the call args -- against
// max_message_size (see send_call in x23/rpc.h). exec_server runs at x23's
// default_max_message_size, which is a runtime-mutable extern (tests lower it)
// with no constexpr to bind to; this mirrors its compile-time default.
// TODO(rstutsman): bind to an x23 constexpr default once x23 exposes one.
constexpr size_t kExecToDbMessageCap = 0x100'0000;

// Bytes of the try_push_new_subtransaction request that count against the cap
// but are NOT per-delta: the rpc_msg header, the BlockID and TransactionID
// args, and the StorageDeltaBatch Vec length prefix. footprint_bound accounts
// for the FlaggedStorageDeltas themselves.
constexpr size_t kMessageOverhead = kXdrMax<x23::rpc_msg> + kXdrMax<BlockID> +
                                    kXdrMax<TransactionID> + kXdrWord;
static_assert(kMessageOverhead < kExecToDbMessageCap);
constexpr size_t kMaxSubtransactionPushFootprint =
    kExecToDbMessageCap - kMessageOverhead;

// Coherence guard: the largest possible single delta must itself fit one push,
// so the per-key/per-tx entry caps can't be raised to a value no push could
// ever carry (which would auto-reject every max-footprint tx). Every
// StorageDelta arm is bounded in the protocol, so xdr_size_bounds derives a
// finite kXdrMax<FlaggedStorageDelta> -- the exact worst case over all arms,
// framing included. (The RUNTIME guard below is the real check: it sums
// footprint_bound over the WHOLE batch, whose key count is unbounded.)
static_assert(kXdrMax<FlaggedStorageDelta> <= kMaxSubtransactionPushFootprint,
              "the largest possible single delta must fit one push");

} // namespace

SystemStatus<bool>
DatabaseProxy::try_push_new_subtransaction(BlockID block_id,
                                           TransactionID tx_id,
                                           StorageDeltaBatch const &delta_batch,
                                           DbConflictResult *conflict_out)
{
  // Reject (as a concurrency-control conflict -- the existing `false`
  // lock-acquisition signal, surfaced as a nondeterministic-tx abort) any batch
  // whose r/w footprint would not fit the RPC message cap, so an oversized push
  // can never reach serialization and turn into a node-unrecoverable error.
  size_t footprint = 0;
  for (auto const &fd : delta_batch) {
    size_t bound = footprint_bound(fd);
    // footprint_bound must be a true upper bound on the serialized size.
    // Validate it against the serializer here, where it is compiled out of the
    // normal path (release strips assert) -- so ser/des stays off the gate
    // while a StorageDelta field/type change that the hand-written bound misses
    // is still caught in tests before it could re-open the overflow.
    assert(bound >= x23::xdr_size(fd) &&
           "footprint_bound under-counts a StorageDelta; update the "
           "footprint_bound overload for the changed delta layout");
    footprint += bound;
  }
  if (footprint + kMessageOverhead > kExecToDbMessageCap) {
    debug(
        "rejecting subtransaction push as a conflict: delta-batch footprint "
        "{} + message overhead {} exceeds the {}-byte executor->HogDB message "
        "cap",
        footprint, kMessageOverhead, kExecToDbMessageCap);
    return false;
  }

  if constexpr (is_debug_build()) {
    for (auto const &delta : delta_batch) {
      check_delta_type(delta.addrkey.key.key_type(), delta.delta.type());
    }
  }
  try {
    // Split the batch so each shard receives only the deltas for the keys it
    // owns, via the shared cross-shard batch core. Every shard gets a
    // (possibly empty) subtransaction so all shards' savepoint stacks stay
    // aligned for pop_subtransactions. This is the prepare phase of the
    // cross-shard 2PC.
    auto split = split_by_shard(
        delta_batch, clients_.size(),
        [](auto const &delta, size_t n) { return shard_of_key(delta.addrkey, n); });
    for (size_t shard = 0; shard < clients_.size(); ++shard) {
      auto res = clients_[shard].try_push_new_subtransaction(
          block_id, tx_id, split.batches[shard]);
      // Stop on the first error or conflict; the caller aborts, which fans the
      // abort out to every shard.
      switch (res.error()) {
      case RPCError::kRPCNone:
        break;
      case RPCError::kRPCStorageWriteFailed:
        if (conflict_out != nullptr) {
          *conflict_out = std::move(res.conflict());
        }
        return false;
      case RPCError::kRPCBadBlockID:
        return SystemError{SystemErrorCode::Abandon};
      default:
        error("Unrecoverable: Received unhandled error type from RPC: {}",
              res.error());
        return SystemError{SystemErrorCode::Unrecoverable};
      }
    }
    return SystemStatus<bool>{true};
  } catch (std::exception const &e) {
    // Same containment as commit_transaction: this runs on a detached executor
    // worker, so contain every failure -- XdrBadInput (oversized delta batch),
    // XdrCallFailed (server-side SYSTEM_ERR), and std::bad_alloc (local OOM) --
    // and return a structured result. The diagnostic allocates and can throw
    // again under memory pressure, so guard it with the no-allocation reporter
    // before returning (cf. syscall_invoke.cc, #2719).
    try {
      error("Unrecoverable: try_push_new_subtransaction failed "
            "(transport/serialization/allocation failure): {}",
            e.what());
    } catch (...) {
      report_dropped_log("try_push_new_subtransaction failure diagnostic");
    }
    return SystemError{SystemErrorCode::Unrecoverable};
  }
}

SystemStatus<void>
DatabaseProxy::pop_subtransactions(BlockID block_id, TransactionID tx_id,
                                   uint32_t count)
{
  // Every shard has an aligned savepoint stack (see try_push_new_subtransaction),
  // so pop `count` from each. Visit ALL shards even after one fails and return
  // the first error: stopping early would leave shards past the failing one a
  // pop deeper, desynchronising the savepoint stacks the next
  // try_push_new_subtransaction relies on (mirrors broadcast_status).
  SystemStatus<void> first_error{};
  for (size_t shard = 0; shard < clients_.size(); ++shard) {
    auto res = clients_[shard].pop_subtransactions(block_id, tx_id, count);
    auto status = to_system_error_code<VoidMarker{}>(res, [] {});
    if (!status.has_value() && first_error.has_value()) {
      first_error = status;
    }
  }
  return first_error;
}

SystemStatus<DbTip>
DatabaseProxy::get_db_tip(BlockID block_id)
{
  auto &client = client_for();
  auto res = client.get_db_tip(block_id);
  return to_system_error_code<NotFoundIsUnrecoverableError{}>(
      res, [block_id](DbTip &&x) -> SystemStatus<DbTip> {
        if (x.id == block_id) {
          error("Unrecoverable: Received DbTip with block id equal to current "
                "block id: {} (should be parent ID)",
                x.id);
          return SystemError{SystemErrorCode::Unrecoverable};
        }
        return x;
      });
}

SystemStatus<ViewingSnapshot>
DatabaseProxy::open_viewing_snapshot()
{
  auto &client = client_for();
  auto res = client.open_viewing_snapshot();
  return to_system_error_code<NotFoundIsUnrecoverableError{}>(
      res,
      [](ViewingSnapshot &&x) -> SystemStatus<ViewingSnapshot> { return x; });
}

SystemStatus<void>
DatabaseProxy::close_viewing_snapshot(BlockID block_id)
{
  auto &client = client_for();
  auto res = client.close_viewing_snapshot(block_id);
  return to_system_error_code<VoidMarker{}>(res, [] {});
}

} // namespace hogd
