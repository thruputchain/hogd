# Auth Map Const-Correctness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the shared `auth_data` map const in the parallel verification lambda and replace `operator[]` lookups with `find()`.

**Architecture:** Single-file change in `auth_server.cc`. The lambda in `verify_transactions()` takes the map by const ref instead of mutable ref, and all three lookup sites switch from `CHECK(contains) + operator[]` to `find() + CHECK(it != end()) + it->second`.

**Tech Stack:** C++, gtest, CMake

---

### Task 1: Fix the parallel lambda const-correctness

**Files:**
- Modify: `hogd/auth/auth_server.cc:235-271`

- [ ] **Step 1: Change lambda parameter to const ref**

In `hogd/auth/auth_server.cc`, change line 236 from:

```cpp
           std::unordered_map<ContractAddress, AddressAuthData> &auth_data,
```

to:

```cpp
           const std::unordered_map<ContractAddress, AddressAuthData> &auth_data,
```

- [ ] **Step 2: Replace account lookup with find()**

Replace lines 246-248:

```cpp
            const ContractAddress &account = tx.tx.invocation.invoked_address;
            CHECK(auth_data.contains(account));
            AddressAuthData &source_auth_data = auth_data[account];
```

with:

```cpp
            const ContractAddress &account = tx.tx.invocation.invoked_address;
            const auto account_it = auth_data.find(account);
            CHECK(account_it != auth_data.end());
            const AddressAuthData &source_auth_data = account_it->second;
```

- [ ] **Step 3: Replace contract lookup with find()**

Replace lines 255-257:

```cpp
            for (const ContractAddress &contract : relevant_contracts) {
              CHECK(auth_data.contains(contract));
              aux_auth_data[contract] = &auth_data[contract];
            }
```

with:

```cpp
            for (const ContractAddress &contract : relevant_contracts) {
              const auto contract_it = auth_data.find(contract);
              CHECK(contract_it != auth_data.end());
              aux_auth_data[contract] = &contract_it->second;
            }
```

- [ ] **Step 4: Build and run existing tests**

Build using the VSCode build config (Debug), then run the auth tests:

```bash
ctest --test-dir ~/build/hogd-debug --output-on-failure --timeout 120 -R "^AuthVerifyTest"
```

Expected: all `AuthVerifyTest` tests pass (VerifyTransaction, VerifyMultipleTransaction).

- [ ] **Step 5: Commit**

```bash
git add hogd/auth/auth_server.cc
git commit -m "fix: const-qualify auth_data in parallel verify_transactions lambda (#1451)

Replace operator[] with find() for const-safe lookups and eliminate
redundant double hash-table lookups (contains + operator[])."
```

### Task 2: Add parallel verification test from issue PoC

**Files:**
- Modify: `hogd/auth/auth_verify_test.cc`

- [ ] **Step 1: Add the parallel test**

Append the following test before the closing `} // namespace hogd` at the end of `hogd/auth/auth_verify_test.cc` (before line 189):

```cpp
TEST_F(AuthVerifyTest, ParallelVerifyTransactionsMapDataRace)
{
  constexpr size_t kNumAddresses = 4;
  constexpr size_t kTxsPerAddress = 16;
  constexpr size_t kTotalTxs = kNumAddresses * kTxsPerAddress;
  constexpr size_t kMaxParallelism = 4;

  ContractAddress addresses[kNumAddresses];
  std::pair<PublicKey, SecretKey> keys[kNumAddresses];

  for (size_t i = 0; i < kNumAddresses; i++) {
    addresses[i] = make_test_address(static_cast<uint8_t>(20 + i));

    auto [pk, sk] = make_key_pair();
    keys[i] = {pk, sk};

    AddressAuthData auth_data{};
    add_pk_to_auth_data(auth_data, 0, pk);
    auth_data.nonce_config.accepted_keys = 1 << 0;
    auth_data.nonce_config.required_weight = 1;
    auth_data.gas_permissions.push_back(GasPermissionConfig{
        .limit = 100, .accepted_keys = 1 << 0, .required_weight = 1});

    mock_db().set(addresses[i], auth_data);
  }

  mock_db().compute_key_changes_for_current_height();
  auth_server().register_auth_data_updates(
      0, mock_db().get_key_changes_at_height(0))();

  x23::Vec<SignedTransaction> stx_vec;
  for (size_t i = 0; i < kTotalTxs; i++) {
    const size_t addr_idx = i % kNumAddresses;

    Transaction tx{};
    tx.invocation.invoked_address = addresses[addr_idx];
    tx.auth_claims.push_back(make_self_individual_key_claim(0));

    SignedTransaction stx{};
    stx.tx = tx;
    stx.signatures.push_back(
        make_signature(keys[addr_idx].second, tx, 0, 0));
    stx_vec.push_back(std::move(stx));
  }

  x23::span<SignedTransaction> stx_span(stx_vec);
  const auto result =
      auth_server().verify_transactions(0, stx_span, kMaxParallelism)();

  ASSERT_EQ(result.size(), kTotalTxs);
  for (AuthErrorCode status : result) {
    EXPECT_EQ(status, AuthErrorCode::kAuthSuccess);
  }
}
```

- [ ] **Step 2: Build and run all auth tests**

Build using the VSCode build config (Debug), then:

```bash
ctest --test-dir ~/build/hogd-debug --output-on-failure --timeout 120 -R "^AuthVerifyTest"
```

Expected: all three tests pass (VerifyTransaction, VerifyMultipleTransaction, ParallelVerifyTransactionsMapDataRace).

- [ ] **Step 3: Commit**

```bash
git add hogd/auth/auth_verify_test.cc
git commit -m "test: add parallel verify_transactions test (#1451)

Exercises the multi-chunk verification path with max_parallelism=4
and 64 transactions across 4 addresses."
```
