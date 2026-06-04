# StorageSnapshotCache contract-entry back-pointer + eager rv32 translation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the deployment cache entry a back-pointer to the contract-code cache entry, store the eagerly-built rv32 translation inside that entry, and let the rv32 engine reach code + ready-to-instantiate image from a single deployment lookup with no second hash lookup.

**Architecture:** `contract_cache_` entries grow from `Contract` to `ContractCacheEntry { Contract; std::optional<Rv32Contract> translated; }`, with the translation built eagerly (calling `Rv32Contract::create`) at insert time. `deployment_cache_` entries become `DeployedContract { DeployedContractHash hash; const ContractCacheEntry* entry; }`. `make_new_runtime` takes `DeployedContract const&`; rv32 uses the back-pointer on the snapshot path and translates on the fly for tx-local deployments (the runtime owns that translation). `Rv32Context::translation_cache_` is removed entirely — the snapshot cache is now the only translation store. `item_objects` is folded into the `runtime` static lib so the cache can call into rv32 directly.

**Tech Stack:** C++23, CMake, GoogleTest. Built objects: `item_objects`, `rv32_runtime_objects`, `runtime` (static lib).

**Spec:** `docs/superpowers/specs/2026-06-03-storage-snapshot-cache-contract-backpointer-design.md`

---

## Design decisions locked during brainstorming

- **Eager** translation, at the moment a `Contract` enters `contract_cache_` (not lazy).
- **Single back-pointer** to `ContractCacheEntry` (not separate code/translation pointers).
- **Direct dependency** from the cache on `Rv32Contract` (no injected translator, no abstract base) — resolved by folding `item_objects` into `runtime`.
- **`DeployedContract` is implicitly constructible from `DeployedContractHash`** (entry = null), so the ~40 `make_new_runtime(deployed_contract_hash, …)` *call sites* compile unchanged; only the virtual signature and its *overrides* change.
- **`Rv32Context::translation_cache_` is removed entirely.** The snapshot/hot path translates eagerly into the snapshot cache and reaches it via the back-pointer. The tx-local fallback translates on the fly and hands ownership to the `Rv32Runtime` (which gains an owning constructor; `Runtime` is non-movable so an interior pointer is stable). Consequence: a tx-local rv32 contract re-translates on each `make_new_runtime` call — acceptable for that rare path, and it removes the unbounded `static thread_local` growth the `TODO(geoff)` flagged.

## Header-cycle constraint (why `deployed_contract.h` is separate)

`storage_snapshot_cache.h` must include `rv32/rv32_contract.h` (to store `std::optional<Rv32Contract>` by value). `rv32_contract.h` includes `runtime.h`. `runtime.h` needs `DeployedContract` for the `make_new_runtime` signature. Defining `DeployedContract` in `storage_snapshot_cache.h` would form the cycle `runtime.h → storage_snapshot_cache.h → rv32_contract.h → runtime.h`. So `DeployedContract` lives in a light new header `deployed_contract.h` that only includes `contract.hh` and forward-declares `ContractCacheEntry`. `runtime.h` includes that; the full `ContractCacheEntry` definition stays in `storage_snapshot_cache.h`.

---

## Task 1: Fold `item_objects` into `runtime`; point `item_test` at `runtime`

Pure build refactor, no code/behavior change. Done first so later tasks can't fail at link when the cache starts referencing rv32 symbols. Currently only two targets link the `item` static lib: `runtime` (`runtime/CMakeLists.txt:142`) and `item_test` (`item/CMakeLists.txt:26`).

**Files:**
- Modify: `hogd/hogexec/runtime/CMakeLists.txt` (the `runtime` STATIC definition + its `target_link_libraries`)
- Modify: `hogd/hogexec/runtime/item/CMakeLists.txt` (drop the `item` STATIC lib; repoint `item_test`)

- [ ] **Step 1: Bake `item_objects` into the `runtime` archive**

In `hogd/hogexec/runtime/CMakeLists.txt`, add the item objects to the `runtime` STATIC target (mirroring wasm/rv32):

```cmake
add_library(runtime STATIC
  $<TARGET_OBJECTS:runtime_objects>
  $<TARGET_OBJECTS:wasm_runtime_objects>
  $<TARGET_OBJECTS:rv32_runtime_objects>
  $<TARGET_OBJECTS:item_objects>
)
```

- [ ] **Step 2: Remove the now-duplicate `item` lib link from `runtime`**

In the same file, in `target_link_libraries(runtime …)` (the block beginning at line ~139), delete the `item` line. `runtime` already links `hogexec_common` and `components` (the `item` lib's only deps), so nothing else moves.

```cmake
target_link_libraries(
  runtime
  hogexec_common
  components
  unified_lib
  protocol
  metrics
  event
  rv32_vm
  hogd
)
```

- [ ] **Step 3: Delete the standalone `item` static lib and repoint `item_test`**

In `hogd/hogexec/runtime/item/CMakeLists.txt`:
- Delete the `add_library(item STATIC $<TARGET_OBJECTS:item_objects>)` line and its `target_link_libraries(item hogexec_common components)` line. (Keep the `item_objects` OBJECT library and its `target_include_directories`.)
- In `target_link_libraries(item_test …)`, replace `item` with `runtime`:

```cmake
target_link_libraries(item_test
  runtime
  runtime_test_util
  hogexec_common
  components
  protocol
  storage_cost
  GTest::gtest
)
```

- [ ] **Step 4: Configure + build everything, run the item + staleness tests**

Run:
```bash
cmake --build build --target item_test runtime_test direct_rpc_tests 2>&1 | tail -30
ctest --test-dir build -R "item_test|DirectStaleness" --output-on-failure 2>&1 | tail -20
```
Expected: configure + build succeed; tests PASS. If `item_test` fails to link with undefined rv32/OpenSSL symbols, add `rv32_vm` to its link line (it should arrive transitively via `runtime`; add explicitly only if the linker disagrees).

- [ ] **Step 5: Commit**

```bash
git add hogd/hogexec/runtime/CMakeLists.txt hogd/hogexec/runtime/item/CMakeLists.txt
git commit -m "build: fold item_objects into runtime lib; item_test links runtime"
```

---

## Task 2: `deployed_contract.h` + `ContractCacheEntry` + eager translation (cache internals)

Introduce the new types and the eager-translation insert path. Keep the old `try_get_deployed_contract_hash` working (as a thin adapter over the new value type) and keep `try_get_contract_by_hash`'s signature, so all existing callers still compile. Build stays green.

**Files:**
- Create: `hogd/hogexec/runtime/item/deployed_contract.h`
- Modify: `hogd/hogexec/runtime/item/storage_snapshot_cache.h`
- Modify: `hogd/hogexec/runtime/item/storage_snapshot_cache.cc`
- Test: `hogd/hogexec/runtime/direct_rpc_tests/direct_staleness_test.cc` (add back-pointer assertions)

- [ ] **Step 1: Create `deployed_contract.h`**

Create `hogd/hogexec/runtime/item/deployed_contract.h`:

```cpp
#pragma once

#include "contract.hh" // DeployedContractHash

namespace hogd {

// Defined in storage_snapshot_cache.h. DeployedContract only holds a pointer
// to it, so a forward declaration suffices here and keeps this header free of
// any rv32 dependency (avoids a runtime.h <-> storage_snapshot_cache.h cycle).
struct ContractCacheEntry;

// A deployed contract: its on-ledger hash metadata plus a borrowed pointer to
// the cached contract entry (code + translation). `entry` is null for
// transaction-local deployments, which are not in the snapshot cache.
struct DeployedContract {
  DeployedContractHash hash;
  const ContractCacheEntry *entry = nullptr;

  // Implicit: lets the many make_new_runtime(DeployedContractHash, ...) call
  // sites and the tx-local fallback construct a DeployedContract with no entry.
  DeployedContract(DeployedContractHash hash) : hash(hash) {}
  DeployedContract(DeployedContractHash hash, const ContractCacheEntry *entry)
    : hash(hash), entry(entry)
  {}
};

} // namespace hogd
```

- [ ] **Step 2: Update `storage_snapshot_cache.h` — includes, types, members, methods**

In `hogd/hogexec/runtime/item/storage_snapshot_cache.h`:

Add includes near the existing ones (`contract.hh` is already included):
```cpp
#include "hogd/hogexec/runtime/item/deployed_contract.h"
#include "hogd/hogexec/runtime/rv32/rv32_contract.h"
```

Add the entry type inside `namespace hogd`, above `class StorageSnapshotCache`:
```cpp
// A cached contract: the bytecode plus, for translatable engines (rv32), its
// eagerly-built ready-to-instantiate image. `translated` is nullopt for
// non-rv32 contracts (and if translation failed — treated as Unrecoverable by
// the engine, matching prior behavior).
struct ContractCacheEntry {
  Contract contract;
  std::optional<Rv32Contract> translated;
};
```

Change the public method declarations:
```cpp
  SystemStatus<const Contract *>
  try_get_contract_by_hash(SHA256Hash const &hash);
  SystemStatus<const DeployedContract *>
  try_get_deployed_contract(ContractAddress const &addr);
  SystemStatus<const SnapshotStorageItem *>
  try_get_snapshot_item(AddressAndKey const &addrkey);
```

Add a private helper declaration (next to `release_db_tip`):
```cpp
  // Lookup/insert/eager-translate the contract entry for `hash`. Returns
  // nullptr (wrapped) for a negative lookup.
  SystemStatus<const ContractCacheEntry *>
  get_contract_entry(SHA256Hash const &hash);
```

Change the two cache member value types:
```cpp
  SynchronizedMap<SHA256Hash, std::optional<ContractCacheEntry>, ByteArrayHasher,
                  std::equal_to<>,
                  std::allocator<std::pair<const SHA256Hash,
                                           std::optional<ContractCacheEntry>>>,
                  std::shared_mutex>
      contract_cache_;

  SynchronizedMap<
      ContractAddress, std::optional<DeployedContract>, ByteArrayHasher,
      std::equal_to<>,
      std::allocator<std::pair<const ContractAddress,
                               std::optional<DeployedContract>>>,
      std::shared_mutex>
      deployment_cache_;
```

- [ ] **Step 3: Rewrite the `.cc` — `get_contract_entry`, `try_get_contract_by_hash`, `try_get_deployed_contract`**

In `hogd/hogexec/runtime/item/storage_snapshot_cache.cc`, replace the existing `try_get_deployed_contract_hash` and `try_get_contract_by_hash` definitions with:

```cpp
SystemStatus<const ContractCacheEntry *>
StorageSnapshotCache::get_contract_entry(SHA256Hash const &hash)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = contract_cache_.find_shared(hash);
    if (obj) {
      return obj->has_value() ? &obj->value() : nullptr;
    }
  }

  auto db_res = db_proxy_.get_contract(block_id_, hash);
  if (!db_res) {
    return db_res.error();
  }

  if constexpr (is_debug_build()) {
    if (db_res.value()) {
      auto received_hash = sha256(*db_res.value());
      if (received_hash != hash) {
        panic("Received incorrect contract hash from db: {} != {}",
              show_byte_array(received_hash), show_byte_array(hash));
      }
    }
  }

  // Build the entry, eagerly translating rv32 contracts so the deployment
  // back-pointer exposes a ready-to-instantiate image with no extra lookup.
  std::optional<ContractCacheEntry> entry;
  if (db_res.value()) {
    Contract contract = std::move(db_res.value()).value();
    std::optional<Rv32Contract> translated;
    if (contract.contract_type() == ContractType::kRv32) {
      translated = Rv32Contract::create(contract.rv32_contract());
    }
    entry.emplace(ContractCacheEntry{.contract = std::move(contract),
                                     .translated = std::move(translated)});
  }

  // TODO(geoff): If we decide to NOT reset the cache after every block,
  // then the contract cache needs to only cache positive lookups
  auto [obj, lock] = contract_cache_.insert(hash, std::move(entry));
  if (obj != nullptr) {
    return obj->has_value() ? &obj->value() : nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = contract_cache_.find(hash, lock);
  CHECK_PTR(obj2);
  return obj2->has_value() ? &obj2->value() : nullptr;
}

SystemStatus<const Contract *>
StorageSnapshotCache::try_get_contract_by_hash(SHA256Hash const &hash)
{
  auto entry = get_contract_entry(hash);
  if (!entry) {
    return entry.error();
  }
  return entry.value() ? &entry.value()->contract : nullptr;
}

SystemStatus<const DeployedContract *>
StorageSnapshotCache::try_get_deployed_contract(ContractAddress const &addr)
{
  {
    // Use shared lock for read-only cache lookup
    auto [obj, sl] = deployment_cache_.find_shared(addr);
    if (obj) {
      return obj->has_value() ? &obj->value() : nullptr;
    }
  }

  auto db_res = db_proxy_.get_deployment(block_id_, addr);
  if (!db_res) {
    return db_res.error();
  }

  std::optional<DeployedContract> deployed;
  if (db_res.value()) {
    DeployedContractHash meta = db_res.value().value();
    // Eagerly warm + translate the contract code; back-point at its entry.
    auto entry = get_contract_entry(meta.hash);
    if (!entry) {
      return entry.error();
    }
    deployed.emplace(meta, entry.value());
  }

  auto [obj, lock] = deployment_cache_.insert(addr, std::move(deployed));
  if (obj != nullptr) {
    return obj->has_value() ? &obj->value() : nullptr;
  }
  CHECK(lock.owns_lock());
  // concurrent insert happened: fetch the result from the cache again
  auto *obj2 = deployment_cache_.find(addr, lock);
  CHECK_PTR(obj2);
  return obj2->has_value() ? &obj2->value() : nullptr;
}
```

Note: `get_deployment` returns `std::optional<DeployedContractHash>`; the
deployment value is copied into `meta` (it is small: 32-byte hash + size +
enum) before the contract lookup. Confirm `<optional>` and `<utility>` are
already included (they are).

- [ ] **Step 4: Add an adapter so existing callers still compile**

Existing callers (`syscall_handler.cc`, `direct_staleness_test.cc`) still call `try_get_deployed_contract_hash`. Keep them green until Task 3 by adding a thin adapter. In `storage_snapshot_cache.h` declare:
```cpp
  // Deprecated shim; removed in the make_new_runtime migration.
  SystemStatus<std::optional<DeployedContractHash>>
  try_get_deployed_contract_hash(ContractAddress const &addr);
```
and in `storage_snapshot_cache.cc`:
```cpp
SystemStatus<std::optional<DeployedContractHash>>
StorageSnapshotCache::try_get_deployed_contract_hash(ContractAddress const &addr)
{
  auto res = try_get_deployed_contract(addr);
  if (!res) {
    return res.error();
  }
  if (res.value() == nullptr) {
    return std::optional<DeployedContractHash>{};
  }
  return std::optional<DeployedContractHash>{res.value()->hash};
}
```

- [ ] **Step 5: Build the cache target and the standalone item test**

Run:
```bash
cmake --build build --target item_test runtime_test 2>&1 | tail -30
```
Expected: builds clean (this is where the new `Rv32Contract::create` symbol must resolve — Task 1 made it available to `item_test` via `runtime`).

- [ ] **Step 6: Add back-pointer assertions to the staleness test (failing first)**

In `hogd/hogexec/runtime/direct_rpc_tests/direct_staleness_test.cc`, after the existing `fresh_contract` block (around line 329), add:

```cpp
  // The deployment entry back-points at the same cached contract entry the
  // direct hash lookup returns, and a native contract has no translation.
  auto fresh_deploy_ptr = cache2.try_get_deployed_contract(contract_addr);
  ASSERT_TRUE(fresh_deploy_ptr.has_value());
  ASSERT_NE(fresh_deploy_ptr.value(), nullptr);
  EXPECT_EQ(&fresh_deploy_ptr.value()->entry->contract,
            fresh_contract.value());
  EXPECT_FALSE(fresh_deploy_ptr.value()->entry->translated.has_value());
```

- [ ] **Step 7: Run the staleness test**

Run:
```bash
ctest --test-dir build -R DirectStaleness --output-on-failure 2>&1 | tail -20
```
Expected: PASS (the existing assertions still use the `_hash` shim; the new ones exercise the back-pointer).

- [ ] **Step 8: Commit**

```bash
git add hogd/hogexec/runtime/item/deployed_contract.h \
        hogd/hogexec/runtime/item/storage_snapshot_cache.h \
        hogd/hogexec/runtime/item/storage_snapshot_cache.cc \
        hogd/hogexec/runtime/direct_rpc_tests/direct_staleness_test.cc
git commit -m "cache: ContractCacheEntry + eager rv32 translation + deployment back-pointer"
```

---

## Task 3: Migrate `make_new_runtime` to `DeployedContract`; rv32 fast path; drop the shim

Change the virtual signature and every override; update `resolve_runtime` and the deploy-collision check; convert the staleness test off the shim; delete the shim. Build is green at the end of the task (it transiently breaks mid-task — do all edits before building).

**Files:**
- Modify (signature, body unchanged via alias): `hogd/hogexec/runtime/runtime.h:186-188`, `hogd/hogexec/runtime/multiplexed_context.h:23` + `multiplexed_context.cc:21-31`, `hogd/hogexec/runtime/native_runtime.h:95` + `native_runtime.cc:67-69`, `hogd/hogexec/runtime/wasm/wasmtime_runtime.h:83` + `wasmtime_runtime.cc:131-133`, `hogd/hogexec/runtime/wasm/wasmi_runtime.h:98` + `wasmi_runtime.cc:141-143`, `hogd/hogexec/runtime/rv32/runtime_context.h:38` + `rv32/runtime_context.cc:149-151`, `hogd/test_ffi/mock_runtime.h:38` + `mock_runtime.cc:50-52`
- Modify (signature + fast path + retire `translation_cache_`): `hogd/hogexec/runtime/rv32/rv32_contract.h` (signature `:65`, `Rv32Runtime` `:35-48`, `Rv32Context` `:50-71`) + `rv32/rv32_contract.cc:191`
- Modify (override signature only, body ignores arg): `hogd/hogexec/runtime/invoke_tx_leak_test.cc:52-58`, `hogd/hogexec/runtime/selective_deployment_test.cc:59-65`
- Modify (callers): `hogd/hogexec/runtime/syscall_handler.cc` (`resolve_runtime` ~942-969; deploy check ~1137-1142)
- Modify (drop shim): `hogd/hogexec/runtime/item/storage_snapshot_cache.{h,cc}`
- Modify (test off shim): `hogd/hogexec/runtime/direct_rpc_tests/direct_staleness_test.cc`

- [ ] **Step 1: Change the base virtual signature**

In `hogd/hogexec/runtime/runtime.h`, replace the `contract.hh` include with the new header if `DeployedContractHash` is no longer otherwise needed (keep `contract.hh` if other declarations use it — it almost certainly does, so just *add* the include):
```cpp
#include "hogd/hogexec/runtime/item/deployed_contract.h"
```
and change:
```cpp
  virtual SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) = 0;
```

- [ ] **Step 2: Migrate each non-rv32 override (mechanical: signature + one alias line)**

For each of these overrides, change the parameter type to `DeployedContract const &deployed_contract` and add, as the **first line of the body**:
```cpp
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
```
The rest of each body is unchanged (it already references `deployed_contract_hash.hash` / `.contract_type`).

Apply to:
- `native_runtime.cc:67` (`NativeContext::make_new_runtime`) — also update the header decl `native_runtime.h:95`.
- `wasm/wasmtime_runtime.cc:131` (`WasmtimeContext::make_new_runtime`) — header `wasmtime_runtime.h:83`.
- `wasm/wasmi_runtime.cc:141` (`WasmiContext::make_new_runtime`) — header `wasmi_runtime.h:98`.
- `rv32/runtime_context.cc:149` (`rv32::RuntimeContext::make_new_runtime`, the mock-test context) — header `rv32/runtime_context.h:38`.

For `multiplexed_context.cc:21` (`MultiplexedContext::make_new_runtime`) — header `multiplexed_context.h:23` — change the signature and forward the **whole struct**:
```cpp
SystemStatus<std::unique_ptr<Runtime>>
MultiplexedContext::make_new_runtime(DeployedContract const &deployed_contract,
                                     SystemCallHandler *syscall_handler)
{
  auto it = contexts_.find(deployed_contract.hash.contract_type);
  if (it == contexts_.end()) {
    return nullptr;
  }
  return it->second.first->make_new_runtime(deployed_contract, syscall_handler);
}
```

For `test_ffi/mock_runtime.cc:50` (`MockRuntimeContext::make_new_runtime`) — header `mock_runtime.h:38` — add the alias line, and change the final forward to pass the whole struct:
```cpp
  return base_context_->make_new_runtime(deployed_contract, syscall_handler);
```

- [ ] **Step 3: Migrate the two test-stub overrides (signature only)**

In `hogd/hogexec/runtime/invoke_tx_leak_test.cc:52` and `hogd/hogexec/runtime/selective_deployment_test.cc:59`, change the override parameter type only (the bodies ignore the argument):
```cpp
  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const & /*unused*/,
                   SystemCallHandler * /*unused*/) override
  {
    return nullptr;
  }
```

- [ ] **Step 4a: `rv32_contract.h` — owning `Rv32Runtime`, drop `translation_cache_`, fix signature**

In `hogd/hogexec/runtime/rv32/rv32_contract.h`:

Give `Rv32Runtime` an owning constructor and an owned member (declared **before** `contract_` so the interior pointer is valid at init). Replace the class body:
```cpp
// One invocation of an RV32 contract. Either borrows a const Rv32Contract*
// owned by the snapshot cache (stable address; snapshot deployments), or owns
// a freshly translated Rv32Contract (transaction-local deployments). Runtime is
// non-movable, so the interior pointer in the owning case stays valid.
class Rv32Runtime final : public Runtime {
public:
  Rv32Runtime(Rv32Contract const *contract, SystemCallHandler *syscall_handler)
    : Runtime(syscall_handler), contract_(contract)
  {}

  Rv32Runtime(Rv32Contract contract, SystemCallHandler *syscall_handler)
    : Runtime(syscall_handler), owned_contract_(std::move(contract)),
      contract_(&owned_contract_.value())
  {}

  ~Rv32Runtime() override = default;

private:
  std::optional<Rv32Contract> owned_contract_;
  Rv32Contract const *contract_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) noexcept override;
};
```
(`invoke_internal` still uses `contract_`, unchanged.)

In `Rv32Context`: change the `make_new_runtime` decl to `DeployedContract const &deployed_contract`, delete the `translation_cache_` member, and rewrite the class doc comment to reflect that translations now live in the snapshot cache:
```cpp
// RuntimeContext for RV32 contracts. Resolves the deployed contract by hash
// like the wasm context. Snapshot deployments arrive with their translation
// already built (in the StorageSnapshotCache, reached via DeployedContract's
// back-pointer); transaction-local deployments are translated on the fly and
// owned by the Rv32Runtime. The context itself holds no per-contract state.
class Rv32Context final : public RuntimeContext {
public:
  InvokeErrorCode verify_contract(Contract const &contract) const override;

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override;
};
```
Remove the now-unused `#include <unordered_map>` (the header's `<optional>` and `threaded_machine.h` includes stay; `Rv32Contract` already lives here).

- [ ] **Step 4b: rv32 fast path + on-the-fly fallback (`rv32/rv32_contract.cc:191`)**

Replace the body of `Rv32Context::make_new_runtime`:

```cpp
SystemStatus<std::unique_ptr<Runtime>>
Rv32Context::make_new_runtime(DeployedContract const &deployed_contract,
                              SystemCallHandler *syscall_handler)
{
  DeployedContractHash const &deployed_contract_hash = deployed_contract.hash;
  if (deployed_contract_hash.contract_type != ContractType::kRv32) {
    error("Unrecoverable: Attempt made to access a non-rv32 contract from "
          "Rv32Context: {}",
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  assert(syscall_handler != nullptr);

  // Fast path: a snapshot deployment carries a back-pointer to the cached
  // contract entry, which already holds the eagerly-built translation. No
  // resolve_contract round trip, no per-invocation translation.
  if (deployed_contract.entry != nullptr &&
      deployed_contract.entry->translated.has_value()) {
    return std::make_unique<Rv32Runtime>(
        &deployed_contract.entry->translated.value(), syscall_handler);
  }

  // Fallback: transaction-local deployment (no cache entry). Resolve the code
  // and translate on the fly; the runtime owns the result for its lifetime.
  auto contract =
      syscall_handler->resolve_contract(deployed_contract_hash.hash);
  if (!contract.has_value()) [[unlikely]] {
    return contract.error();
  }
  if (contract.value() == nullptr) [[unlikely]] {
    error("Unrecoverable: Contract lookup failed: {}",
          show_byte_array(deployed_contract_hash.hash));
    return SystemError{SystemErrorCode::Unrecoverable};
  }
  if (contract.value()->contract_type() != deployed_contract_hash.contract_type)
      [[unlikely]] {
    error("Unrecoverable: Mismatch in contract type: {} != {}",
          contract.value()->contract_type(),
          deployed_contract_hash.contract_type);
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  std::optional<Rv32Contract> translated =
      Rv32Contract::create(contract.value()->rv32_contract());
  if (!translated.has_value()) [[unlikely]] {
    error("Unrecoverable: failed to translate rv32 contract: {}",
          show_byte_array(deployed_contract_hash.hash));
    return SystemError{SystemErrorCode::Unrecoverable};
  }

  return std::make_unique<Rv32Runtime>(std::move(*translated), syscall_handler);
}
```

- [ ] **Step 5: Update `resolve_runtime` (`syscall_handler.cc` ~942-969)**

Replace the body:
```cpp
SystemStatus<std::unique_ptr<Runtime>>
SystemCallHandler::resolve_runtime(ContractAddress const &addr)
{
  // Check the snapshot for a deployed contract; on a hit, the returned entry
  // carries the back-pointer to its (eagerly translated) code.
  auto deployed_expect = snapshot_cache_.try_get_deployed_contract(addr);
  if (!deployed_expect) {
    return deployed_expect.error();
  }
  const DeployedContract *deployed = *deployed_expect;
  if (deployed != nullptr) {
    return runtime_context_.make_new_runtime(*deployed, this);
  }

  // Not in the snapshot; check the transaction's local context.
  auto local_hash =
      invocation_frame_stack_.lookup_locally_deployed_contract(addr);
  if (!local_hash) {
    return nullptr;
  }
  // Tx-local deployment: no cache entry (entry stays null; the engine falls
  // back to resolve_contract).
  return runtime_context_.make_new_runtime(DeployedContract{*local_hash}, this);
}
```

- [ ] **Step 6: Update the deploy-collision check (`syscall_handler.cc` ~1137-1142)**

```cpp
  auto deploy_check_status =
      snapshot_cache_.try_get_deployed_contract(deploy_addr);
  if (!deploy_check_status) {
    return deploy_check_status.error();
  }
  if (*deploy_check_status != nullptr) {
    return SyscallError{
        SyscallErrorCode::kSyscallErrorCodeContractAlreadyDeployed};
  }
```

- [ ] **Step 7: Convert the staleness test off the shim**

In `direct_staleness_test.cc`, replace the three `try_get_deployed_contract_hash` uses (lines ~297, ~313, ~322) with `try_get_deployed_contract`, adjusting the optional checks to pointer checks:
```cpp
  // Warm negative deployment lookup.
  auto warm_deploy = cache1.try_get_deployed_contract(contract_addr);
  ASSERT_TRUE(warm_deploy.has_value());
  EXPECT_EQ(warm_deploy.value(), nullptr);
```
```cpp
  auto stale_deploy = cache1.try_get_deployed_contract(contract_addr);
  ASSERT_TRUE(stale_deploy.has_value());
  EXPECT_EQ(stale_deploy.value(), nullptr);
```
```cpp
  auto fresh_deploy = cache2.try_get_deployed_contract(contract_addr);
  ASSERT_TRUE(fresh_deploy.has_value());
  EXPECT_NE(fresh_deploy.value(), nullptr);
```
(The back-pointer block added in Task 2 already uses the new method.)

- [ ] **Step 8: Delete the shim**

Remove the `try_get_deployed_contract_hash` declaration from `storage_snapshot_cache.h` and its definition from `storage_snapshot_cache.cc` (added in Task 2 Step 4).

- [ ] **Step 9: Build everything and run the affected suites**

Run:
```bash
cmake --build build 2>&1 | tail -40
ctest --test-dir build -R "item_test|DirectStaleness|runtime_test|rv32|invoke_tx_leak|SelectiveDeployment|mock" --output-on-failure 2>&1 | tail -40
```
Expected: full build succeeds; tests PASS. Common failure: a missed `make_new_runtime` override or call site — grep `grep -rn "make_new_runtime" --include=*.cc --include=*.h hogd | grep -v build` and confirm every *override* uses `DeployedContract const&` and every *call site* passes either a `DeployedContract` or a `DeployedContractHash` (implicitly converted).

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "runtime: thread DeployedContract through make_new_runtime; rv32 uses cache back-pointer, drop translation_cache_"
```

---

## Task 4: Test that rv32 translation is eagerly populated via the back-pointer

Lock in the core new behavior: a deployed **real `kRv32`** contract's cache entry has `translated` set, reachable through `try_get_deployed_contract`. Use `rv32_contract_test.cc` — its `Rv32FibonacciTest` deploys real `kRv32` artifacts through `Rv32Context` (the mock `rv32_runtime_context_test.cc` uses `kMockTest` contracts, which are never translated, so it is the wrong place).

The `Rv32FibonacciTest` fixture (`rv32_contract_test.cc:29`) derives from `NativeContractTestFramework`, which provides `get_storage_snapshot_cache()` (returns `std::shared_ptr<StorageSnapshotCache>`, header `native_contract_test_framework.h:349`), and a `deploy_rv32(artifact, addr_seed)` helper (`:48`) that writes a `kRv32` deployment and returns its `ContractAddress`. The deployment is visible to that snapshot cache at the current block immediately (the existing `FibonacciCalls` test deploys then invokes through the same cache without advancing).

**Files:**
- Test: `hogd/hogexec/runtime/rv32/rv32_contract_test.cc`

- [ ] **Step 1: Add the test (passes once Task 3 is in; guards against regression)**

Add this `TEST_P` next to `FibonacciCalls` (after the `Rv32FibonacciTest` fixture, ~line 79):

```cpp
// The deployment back-pointer exposes the eagerly-built rv32 translation,
// so the engine reaches a ready-to-instantiate image with no extra lookup.
TEST_P(Rv32FibonacciTest, DeploymentBackPointerCarriesEagerTranslation)
{
  auto contract_addr = deploy_rv32("fibonacci.rv32a", 0);

  auto cache = get_storage_snapshot_cache();
  auto deployed = cache->try_get_deployed_contract(contract_addr);
  ASSERT_TRUE(deployed.has_value());
  ASSERT_NE(deployed.value(), nullptr);
  ASSERT_NE(deployed.value()->entry, nullptr);
  EXPECT_EQ(deployed.value()->hash.contract_type, ContractType::kRv32);
  EXPECT_TRUE(deployed.value()->entry->translated.has_value());
  // Back-pointer aliases the contract-cache entry the hash lookup returns.
  auto by_hash = cache->try_get_contract_by_hash(deployed.value()->hash.hash);
  ASSERT_TRUE(by_hash.has_value());
  EXPECT_EQ(&deployed.value()->entry->contract, by_hash.value());
}
```

- [ ] **Step 2: Build and run the rv32 contract tests**

Run:
```bash
cmake --build build --target rv32_contract_test 2>&1 | tail -20
ctest --test-dir build -R "Rv32Fibonacci" --output-on-failure 2>&1 | tail -20
```
Expected: PASS. (If the target name differs, find it: `grep -rn "rv32_contract_test\|add_executable" hogd/hogexec/runtime/rv32/CMakeLists.txt`.)

- [ ] **Step 3: Commit**

```bash
git add hogd/hogexec/runtime/rv32/rv32_contract_test.cc
git commit -m "test: rv32 deployment back-pointer carries eager translation"
```

---

## Final verification

- [ ] **Full build + full test sweep**

Run:
```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -40
```
Expected: clean build, all tests PASS.

- [ ] **Confirm no stragglers**

Run:
```bash
grep -rn "try_get_deployed_contract_hash" hogd --include=*.cc --include=*.h | grep -v build
grep -rn "make_new_runtime" hogd --include=*.cc --include=*.h | grep -v build | grep "DeployedContractHash const"
```
Expected: no output from either (the shim is gone; no override still takes `DeployedContractHash const&`).

## Notes carried from the spec

- `ThreadedMachinePreInstantiation` is now shared across threads (a `const Rv32Contract*` borrowed from the cross-thread snapshot cache). Confirm during Task 3 review that `Rv32Contract::instantiate() const` and its `ThreadedMachinePreInstantiation` touch no mutable shared state.
- Trade-off accepted: translations reset per block (block-GC) instead of living in the unbounded `static thread_local` cache. `Rv32Context::translation_cache_` is removed entirely (this PR); tx-local deployments re-translate per `make_new_runtime` and the `Rv32Runtime` owns the result.
- Eager translation fires whenever an rv32 `Contract` enters `contract_cache_`, including a contract fetched only for deploy-validation. Accepted.
