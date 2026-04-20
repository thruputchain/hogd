# Auth Server Map Const-Correctness Fix

**Date**: 2026-04-15
**Issue**: #1451
**Severity**: Low (const-correctness cleanup, not a practical data race)

## Summary

`AuthServer::verify_transactions()` populates a
`std::unordered_map<ContractAddress, AddressAuthData>` sequentially, then passes
it by mutable reference to parallel worker coroutines. The workers only read from
the map, but use `operator[]` (non-const) instead of a const-safe API. This is
not a practical bug on any real stdlib implementation, but is a const-correctness
issue that should be cleaned up.

## Changes

All changes are in `hogd/auth/auth_server.cc`, within the parallel lambda in
`verify_transactions()` (lines ~235-271).

### 1. Lambda signature: take const reference

Change the lambda parameter from:
```cpp
std::unordered_map<ContractAddress, AddressAuthData> &auth_data
```
to:
```cpp
const std::unordered_map<ContractAddress, AddressAuthData> &auth_data
```

### 2. Replace `CHECK(contains) + operator[]` with `find()`

For the account lookup (currently lines 247-248):
```cpp
// Before:
CHECK(auth_data.contains(account));
AddressAuthData &source_auth_data = auth_data[account];

// After:
const auto account_it = auth_data.find(account);
CHECK(account_it != auth_data.end());
const AddressAuthData &source_auth_data = account_it->second;
```

For the contract lookups in the inner loop (currently lines 255-257):
```cpp
// Before:
CHECK(auth_data.contains(contract));
aux_auth_data[contract] = &auth_data[contract];

// After:
const auto contract_it = auth_data.find(contract);
CHECK(contract_it != auth_data.end());
aux_auth_data[contract] = &contract_it->second;
```

### 3. Const-qualify `source_auth_data`

Change `AddressAuthData &source_auth_data` to
`const AddressAuthData &source_auth_data`. This already matches what
`verify_auth()` expects (`const AddressAuthData &`).

## Benefits

- Compiler enforces read-only access to `auth_data` in the parallel section
- Eliminates double hash-table lookups (`contains` + `operator[]` becomes single
  `find()`)
- No behavioral change — the `CHECK` still fires on missing keys

## Testing

The existing `AuthVerifyTest` suite covers the verification paths. The PoC test
from the issue (`ParallelVerifyTransactionsMapDataRace`) can be added to confirm
the parallel path works correctly with the const-safe API.
