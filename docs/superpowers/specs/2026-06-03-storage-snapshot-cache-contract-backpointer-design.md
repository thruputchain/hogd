# StorageSnapshotCache: deployment → contract-entry back-pointer + eager rv32 translation

## Problem

`StorageSnapshotCache` keeps two independent caches:

- `deployment_cache_`: `ContractAddress → std::optional<DeployedContractHash>`
  (metadata only: `{hash, contract_size, contract_type}`).
- `contract_cache_`: `SHA256Hash → std::optional<const Contract>` (the bytecode).

A positive deployment lookup is almost always followed by a contract-code
lookup keyed by the deployment's `hash` — a second `SynchronizedMap` hash +
shared-lock round trip. Concretely, `SystemCallHandler::resolve_runtime` looks
up the deployment, hands `DeployedContractHash` to `make_new_runtime`, and every
`*Context::make_new_runtime` then calls `syscall_handler->resolve_contract(hash)`
to re-fetch the same code it could have reached directly.

Separately, the rv32 engine memoizes the expensive translation (artifact SHA256
verification + program decode/fusion → `Rv32Contract`) in
`Rv32Context::translation_cache_`. That context is a `static thread_local`
(`precompiles/context.cc`), so the translation cache is **per-thread**,
**never evicted**, and **persists across blocks** — unbounded growth, and
redundant translation across worker threads. `rv32_contract.h` already carries a
`TODO(geoff)` to move this into the block-expiring `StorageSnapshotCache`.

## Goals

1. Give the deployment-cache entry a back-pointer to the cached contract entry,
   so a positive deployment lookup yields the code (and more) with no second
   lookup.
2. Move the rv32 translation into the contract entry, built **eagerly** when the
   `Contract` enters the cache. This gives the rv32 hot path code + ready-to-
   instantiate image from a single back-pointer, and ties translation lifetime
   to the block (cross-thread dedup, bounded memory, GC on `reset()`).
3. Keep the change MVP-scoped: rv32 is the only engine that consumes the new
   pointers in this pass; wasmi/wasmtime/native/mock keep their current behavior
   (wasmtime keeps its Rust-side `InstancePre` cache).

## Non-goals

- LRU / cross-block retention of contracts or translations (translations now
  reset per block — accepted; see Trade-offs).
- Wiring the code/translation pointers into wasm/native engines (deferred).
- Changing the FFI `resolve_contract` callback signature.

## Design

### Why a direct dependency is fine

`storage_snapshot_cache` (in `runtime/item/`) will include and use
`rv32/rv32_contract.h` directly. There is no header cycle (no rv32 header
includes the cache), and `item/`, `rv32/`, and `runtime/` objects all link into
the single `runtime` static library. No abstraction layer or dependency
injection is warranted.

### Contract cache entry (owns code + eager translation)

```cpp
struct ContractCacheEntry {
  Contract contract;
  std::optional<Rv32Contract> translated;  // set for kRv32 at insert; nullopt otherwise
};
```

`contract_cache_` value type becomes `std::optional<ContractCacheEntry>`
(nullopt still means "not found", preserving negative-lookup caching).

A private helper centralizes lookup/insert/translate:

```cpp
SystemStatus<const ContractCacheEntry*> get_contract_entry(SHA256Hash const&);
```

On a miss it fetches the bytecode from the db, and if
`contract.contract_type() == kRv32` it eagerly calls
`Rv32Contract::create(contract.rv32_contract())`, storing the result in
`translated`. Other contract types leave `translated` empty. The existing
debug-build hash verification and the insert-race handling (insert; on a lost
race re-find under the held lock and return the canonical pointer) are preserved.

`try_get_contract_by_hash` becomes a thin wrapper returning `&entry->contract`
(or `nullptr`), so the FFI `resolve_contract` path is unchanged.

### Deployment cache entry (back-pointer to the contract entry)

```cpp
struct DeployedContract {
  DeployedContractHash hash;        // {hash, contract_size, contract_type}
  const ContractCacheEntry* entry;  // null only for tx-local deployments
};
```

`deployment_cache_` value type becomes `std::optional<DeployedContract>`
(nullopt = not deployed). The lookup:

```cpp
SystemStatus<const DeployedContract*>
  try_get_deployed_contract(ContractAddress const&);   // nullptr = not deployed
```

replaces `try_get_deployed_contract_hash`. On a miss it fetches the
`DeployedContractHash` from the db; if present it calls `get_contract_entry`
(eager code fetch + eager translation), then caches
`DeployedContract{meta, entry}`. It mirrors the contract-cache insert-race
handling so the returned pointer is always the canonical cached one.

Both caches are cleared together in `reset()`, so the `entry` back-pointer (and
the `Contract`/`Rv32Contract` addresses it transitively exposes) can never
dangle — `unordered_map` nodes are address-stable until erase, and erase only
happens via the shared `reset()`.

### Threading `DeployedContract` into `make_new_runtime`

The virtual signature (`runtime.h`) and all implementations change from
`DeployedContractHash const&` to `DeployedContract const&`.

- **rv32** (`rv32_contract.cc`): use `dc.entry->translated` directly — zero
  extra lookups and zero per-invoke translation on the snapshot path. Fall back
  to `resolve_contract(dc.hash.hash)` + on-the-fly `Rv32Contract::create` only
  when `dc.entry == nullptr` (tx-local deployments deployed and invoked within
  the same transaction). Delete `Rv32Context::translation_cache_`.
- **wasmi / wasmtime / native / mock**: add `auto const& dch = dc.hash;` at the
  top; bodies otherwise unchanged. They ignore `dc.entry` this pass. wasmtime
  keeps its Rust-side `InstancePre` cache.
- `MultiplexedContext::make_new_runtime` forwards `dc`, dispatching on
  `dc.hash.contract_type`.

### Callers

- `SystemCallHandler::resolve_runtime`: call `try_get_deployed_contract(addr)`.
  On a snapshot hit, pass the cached `DeployedContract` through to
  `make_new_runtime`. On the tx-local fallback
  (`invocation_frame_stack_.lookup_locally_deployed_contract`), construct
  `DeployedContract{*local_hash, nullptr}`.
- Deploy-collision check (`syscall_handler.cc:~1138`): logic unchanged — it only
  reads the returned pointer for existence (it expects *absence*).

### Tests

- `direct_rpc_tests/direct_staleness_test.cc`: update to the new method name /
  pointer return.
- All `make_new_runtime(deployed_contract_hash, ...)` call sites in `*_test.cc`
  wrap the argument as `DeployedContract{hash, nullptr}` (a small test helper may
  reduce churn).

## To verify during implementation

- `ThreadedMachinePreInstantiation` (inside `Rv32Contract`) has no mutable shared
  state, so a `const Rv32Contract*` is safe to share across threads for
  concurrent `instantiate() const`. Required because the translation now lives in
  the cross-thread-shared snapshot cache rather than a thread-local context.

## Trade-offs

- **Lose cross-block translation warmth, gain cross-thread dedup + bounded
  memory.** Translations now reset per block instead of living forever in a
  thread-local map. This matches the existing `TODO(geoff)` intent.
- **Eager translation is tied to contract-cache insertion**, so any rv32
  `Contract` that enters the cache is translated immediately — including one
  fetched only for deploy-validation (`syscall_handler.cc:~1118`) even if never
  invoked. Accepted: such a contract is almost always about to be invoked.
- **Redundant translation on a concurrent insert race**: two threads fetching the
  same contract simultaneously both translate; the insert loser discards. This
  matches the existing redundant-db-fetch behavior on the same race and can be
  tightened later if it matters.
