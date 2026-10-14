# StorageCostModel Schema Narrowing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Narrow StorageCostModelXdr fields from int128_xdr to unsigned hyper (uint64) to eliminate signed-overflow UB in cost calculations.

**Architecture:** Change the XDR schema, rebuild generated headers, then update StorageCostModel and all callsites to use plain uint64/uint32 values instead of int128_xdr wrappers.

**Tech Stack:** C++, XDR (x23c compiler), CMake, GTest

---

### Task 1: Change the XDR schema

**Files:**
- Modify: `protocol/storage_cost.x`

- [ ] **Step 1: Update field types**

Replace the int128_xdr fields with unsigned hyper and int with unsigned int:

```xdr
struct StorageCostModelXdr {
  ExtensionPoint ext;
  unsigned hyper base_data_item_cost;
  unsigned int bytestring_cost_per_byte;
  unsigned hyper base_contract_deployment_cost;
  unsigned hyper contract_deployment_cost_per_byte;
  unsigned hyper auth_per_key_cost;
  unsigned hyper rcq_cost_per_entry;
  unsigned hyper replay_cache_cost_per_entry;
  unsigned hyper balance_cost;
};
```

- [ ] **Step 2: Rebuild to regenerate headers**

Run:
```bash
cmake --build ./build -j16
```

Expected: build fails with type errors in StorageCostModel and test files (because they still use `to_xdr(int128_t{...})` for fields that are now `uint64_t`). This confirms the schema change propagated.

- [ ] **Step 3: Commit**

```bash
git add protocol/storage_cost.x
git commit -m "xdr: narrow StorageCostModelXdr fields to unsigned hyper/int

Closes the attack surface for issues #2093, #2083, #2076 where
int128_xdr fields allowed overflow-range or negative cost values."
```

### Task 2: Update StorageCostModel header

**Files:**
- Modify: `hogd/storage_cost/storage_cost_model.h`

- [ ] **Step 1: Update XDR constructor to drop from_xdr() calls**

The XDR fields are now plain uint64_t/uint32_t, so the constructor just assigns directly (implicit widening to int128_t):

```cpp
  constexpr StorageCostModel(StorageCostModelXdr const &model)
    : kBaseItemCost(model.base_data_item_cost),
      kBytestringCostPerByte(model.bytestring_cost_per_byte),
      kBaseContractDeploymentCost(model.base_contract_deployment_cost),
      kContractDeploymentCostPerByte(model.contract_deployment_cost_per_byte),
      kAuthPerKeyCost(model.auth_per_key_cost),
      kRCQCostPerEntry(model.rcq_cost_per_entry),
      kReplayCacheCostPerByte(model.replay_cache_cost_per_entry),
      kBalanceCost(model.balance_cost)
  {}
```

- [ ] **Step 2: Change kBytestringCostPerByte member type from int32_t to uint32_t**

```cpp
  const uint32_t kBytestringCostPerByte;
```

- [ ] **Step 3: Update default_cost_model() to drop to_xdr() wrappers**

```cpp
  static constexpr StorageCostModel default_cost_model()
  {
    return StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = 12000,
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = 12000,
        .contract_deployment_cost_per_byte = 10,
        .auth_per_key_cost = 1111,
        .rcq_cost_per_entry = 8,
        .replay_cache_cost_per_entry = 7,
        .balance_cost = 1234,
    });
  }
```

- [ ] **Step 4: Commit**

```bash
git add hogd/storage_cost/storage_cost_model.h
git commit -m "storage_cost: update StorageCostModel for uint64 XDR fields

Drop from_xdr() calls in XDR constructor, narrow kBytestringCostPerByte
to uint32_t, simplify default_cost_model() literals."
```

### Task 3: Update StorageCostModel to_xdr()

**Files:**
- Modify: `hogd/storage_cost/storage_cost_model.cc`

- [ ] **Step 1: Update to_xdr() to use static_cast instead of ::hogd::to_xdr()**

The XDR fields are now uint64_t/uint32_t, so narrow with static_cast:

```cpp
StorageCostModelXdr
StorageCostModel::to_xdr() const
{
  return StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = static_cast<uint64_t>(kBaseItemCost),
      .bytestring_cost_per_byte = kBytestringCostPerByte,
      .base_contract_deployment_cost =
          static_cast<uint64_t>(kBaseContractDeploymentCost),
      .contract_deployment_cost_per_byte =
          static_cast<uint64_t>(kContractDeploymentCostPerByte),
      .auth_per_key_cost = static_cast<uint64_t>(kAuthPerKeyCost),
      .rcq_cost_per_entry = static_cast<uint64_t>(kRCQCostPerEntry),
      .replay_cache_cost_per_entry =
          static_cast<uint64_t>(kReplayCacheCostPerEntry),
      .balance_cost = static_cast<uint64_t>(kBalanceCost),
  };
}
```

- [ ] **Step 2: Commit**

```bash
git add hogd/storage_cost/storage_cost_model.cc
git commit -m "storage_cost: update to_xdr() for uint64 fields"
```

### Task 4: Update test files

**Files:**
- Modify: `hogd/storage_cost/storage_cost_model_test.cc`
- Modify: `hogd/hogdb/hogdb_test.cc:82-100`
- Modify: `hogd/hogdb/item_change_auth_test.cc:75-81`
- Modify: `hogd/hogdb/recovery_tests.cc:62-68`
- Modify: `hogd/mempool/test/infra.h:215-221`

- [ ] **Step 1: Update storage_cost_model_test.cc**

Replace all `to_xdr(int128_t{N})` with plain integer literals in StorageCostModelXdr constructions. For example, in `RoundtripCustomValues`:

```cpp
  StorageCostModelXdr original{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 42,
      .bytestring_cost_per_byte = 7,
      .base_contract_deployment_cost = 100,
      .contract_deployment_cost_per_byte = 3,
      .auth_per_key_cost = 55,
      .rcq_cost_per_entry = 11,
      .replay_cache_cost_per_entry = 17,
      .balance_cost = 9999,
  };
```

And in `ContractDeploymentCostIncludesBase`:

```cpp
  StorageCostModelXdr xdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 5000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 12000,
      .contract_deployment_cost_per_byte = 10,
      .auth_per_key_cost = 1111,
      .rcq_cost_per_entry = 8,
      .replay_cache_cost_per_entry = 7,
      .balance_cost = 1234,
  };
```

- [ ] **Step 2: Update hogdb_test.cc cost_model_ and cost_model_with_balance_cost_**

```cpp
  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 12000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 12000,
      .contract_deployment_cost_per_byte = 10,
      .replay_cache_cost_per_entry = 7,
  });

  StorageCostModel cost_model_with_balance_cost_ =
      StorageCostModel(StorageCostModelXdr{
          .ext = ExtensionPoint(0),
          .base_data_item_cost = 12000,
          .bytestring_cost_per_byte = 10,
          .base_contract_deployment_cost = 12000,
          .contract_deployment_cost_per_byte = 10,
          .replay_cache_cost_per_entry = 7,
          .balance_cost = 1000,
      });
```

- [ ] **Step 3: Update item_change_auth_test.cc cost_model_**

```cpp
  StorageCostModel cost_model_ = StorageCostModel(StorageCostModelXdr{
      .ext = ExtensionPoint(0),
      .base_data_item_cost = 1000,
      .bytestring_cost_per_byte = 10,
      .base_contract_deployment_cost = 1000,
      .contract_deployment_cost_per_byte = 10,
  });
```

- [ ] **Step 4: Update recovery_tests.cc cost model construction**

```cpp
    ctx.initialize_cost_model(StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = 12000,
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = 12000,
        .contract_deployment_cost_per_byte = 10,
    }));
```

- [ ] **Step 5: Update mempool/test/infra.h cost model construction**

```cpp
    ctx_->initialize_cost_model(StorageCostModel(StorageCostModelXdr{
        .ext = ExtensionPoint(0),
        .base_data_item_cost = 12000,
        .bytestring_cost_per_byte = 10,
        .base_contract_deployment_cost = 12000,
        .contract_deployment_cost_per_byte = 10,
    }));
```

- [ ] **Step 6: Build and run tests**

```bash
cmake --build ./build -j16
ctest --test-dir ./build -R StorageCostModel -V
```

Expected: all StorageCostModel tests pass.

- [ ] **Step 7: Run full test suite**

```bash
ctest --test-dir ./build -j16
```

Expected: no regressions.

- [ ] **Step 8: Commit**

```bash
git add hogd/storage_cost/storage_cost_model_test.cc hogd/hogdb/hogdb_test.cc hogd/hogdb/item_change_auth_test.cc hogd/hogdb/recovery_tests.cc hogd/mempool/test/infra.h
git commit -m "test: update StorageCostModelXdr literals for uint64 fields"
```
