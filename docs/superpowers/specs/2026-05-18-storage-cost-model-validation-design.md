# StorageCostModel Schema Narrowing

## Problem

StorageCostModel accepts arbitrary signed int128_t values from XDR without
validation. Three open issues (#2093, #2083, #2076) exploit this to trigger
signed-overflow undefined behavior in the `get_*_cost()` methods via
adversarial genesis configs (e.g. `auth_per_key_cost = INT128_MIN`,
`rcq_cost_per_entry = INT128_MAX`).

## Solution

Narrow the XDR schema: change all `int128_xdr` cost fields in
`StorageCostModelXdr` to `unsigned hyper` (uint64), and `int
bytestring_cost_per_byte` to `unsigned int` (uint32). This makes both negative
and overflow-range values unrepresentable in the wire format. No runtime
validation needed.

## Why uint64 is safe

The worst realistic multiplication is `size_t * uint64_t`. The result fits
comfortably in int128_t for any practical count value. The default cost model
values are all tiny (max 12000).

## XDR schema change

`protocol/storage_cost.x`:

```xdr
struct StorageCostModelXdr {
  ExtensionPoint ext;
  unsigned hyper base_data_item_cost;               // was int128_xdr
  unsigned int bytestring_cost_per_byte;            // was int
  unsigned hyper base_contract_deployment_cost;      // was int128_xdr
  unsigned hyper contract_deployment_cost_per_byte;  // was int128_xdr
  unsigned hyper auth_per_key_cost;                  // was int128_xdr
  unsigned hyper rcq_cost_per_entry;                 // was int128_xdr
  unsigned hyper replay_cache_cost_per_entry;        // was int128_xdr
  unsigned hyper balance_cost;                       // was int128_xdr
};
```

## StorageCostModel C++ changes

- Keep member fields as int128_t (widened losslessly from uint64 on
  construction). This avoids churn in arithmetic and return types.
- XDR constructor: drop `from_xdr()` calls (fields are now plain integers).
- `to_xdr()`: drop `::hogd::to_xdr()` wrappers, narrow back to uint64_t.
- `kBytestringCostPerByte`: change from int32_t to uint32_t.
- `default_cost_model()`: drop `::hogd::to_xdr()` wrappers on literal values.
- No `is_valid()` needed.

## Code changes

1. `protocol/storage_cost.x`: int128_xdr -> unsigned hyper, int -> unsigned int
2. `storage_cost_model.h`: update XDR constructor (drop from_xdr), update
   member type for kBytestringCostPerByte, update default_cost_model()
3. `storage_cost_model.cc`: update to_xdr() to drop wrappers
4. `storage_cost_model_test.cc`: update test XDR literals
5. Update other test files that construct StorageCostModelXdr literals

## Issues closed

- #2093 Auth Negative Cost Release Overflow
- #2083 Retrocausal Queue Storage Cost Signed Overflow
- #2076 Deployment Storage Cost Signed Overflow
