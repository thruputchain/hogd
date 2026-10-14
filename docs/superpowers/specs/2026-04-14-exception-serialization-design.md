# Exception Serialization Format Update

## Problem

The SDK now serializes contract exceptions as `(u64 selector, CXS body)` instead of the old `CrossExn` format `(i32 discriminant, CXS body)`. A new `contract_result.h` in `hogd/hogexec/components/` implements `Result<T, ContractException<ExnType, Selector>...>` and `ContractErrorType` to handle this. The old `runtime/contract_result.h` needs to be deleted and all consumers updated.

## Design

### 1. Delete old runtime/contract_result.h

Delete `hogd/hogexec/runtime/contract_result.h`. Update 6 files that include it to use `hogd/hogexec/components/contract_result.h`.

Files to update:
- `hogd/hogexec/runtime/contract_trait.h`
- `hogd/hogexec/precompiles/precompile_native_token.cc`
- `hogd/hogexec/precompiles/precompile_initial_token_claim.cc`
- `hogd/hogexec/runtime/manual_revert_wasm_test.cc`
- `hogd/hogexec/runtime/contract_result_preset_handling_test.cc`
- `hogd/hogexec/precompiles/native_contract_builder.h`

### 2. Fix include paths in new contract_result.h

The new `components/contract_result.h` includes `library_exception.hh` and `transaction.hh` with bare names. These are generated headers in the binary dir. Fix to full paths:
- `"library_exception.hh"` -> `"hogd/hogexec/components/library_exception.hh"`
- `"transaction.hh"` -> `"hogd/xdr/transaction.hh"`

### 3. Update contract_trait.h decode_return

Replace the old CrossExn parsing (lines 220-287):

```
Old: load i32 discriminant, check == 0, deserialize err_type from remaining
New: construct err_type from raw ReturnBuffer (ContractErrorType parses selector+body internally)
```

The new `ContractErrorType` constructor takes a `ReturnBuffer&&` and handles `(u64 selector, Buf body)` parsing, dispatching to the correct `ContractException<T, Sel>` variant.

Remove `kCrossExnContractDiscriminant`.

### 4. Update native_contract_builder.h revert encoding

The `do_revert` lambda writes the abort buffer for precompile errors. Change from:
```cpp
// Old: i32 discriminant + CXS body
x23::store_cxs_int<int32_t>(buf, kCrossExnContractDiscriminant);
buf.append(body);
```
To:
```cpp
// New: CXS-encoded (u64 selector, Buf body) tuple
// ContractErrorType expects this format
```

The selector is available via the `ContractException::kSelector` on the return type's error variants. Since `native_contract_builder.h` is generic over the contract trait, it can extract the selector from the `err_type` template parameter.

### 5. Update generate_header.py

Currently generates:
```cpp
Result<void, ERC20Error>
```

Change to:
```cpp
Result<void, ContractException<ERC20Error, 0xb19947acb2fa14ffULL>>
```

The selector comes from `spec["interfaces"][0]["throws"][i]["selector"]`. Each throw entry becomes a `ContractException<Name, Selector>` template argument. Interfaces with multiple throws get multiple `ContractException` args in the `Result`.

### 6. Update precompile Result types

`precompile_native_token.cc` and `precompile_initial_token_claim.cc` use `Result<T, itc_erc20::ERC20Error>`. These change to match the generated header's type, e.g. `Result<T, ContractException<itc_erc20::ERC20Error, 0x...ULL>>`.

Since these files use the `itc_erc20::ERC20` trait from the generated header, and that header's `export_types` defines the full `Result` type, the precompiles can use a type alias from the trait rather than spelling out the `ContractException` manually.

### 7. Test updates

- `contract_result_preset_handling_test.cc` — update for new `Result<T, Exns...>` signature
- `manual_revert_wasm_test.cc` — update for new exception parsing format

These tests verify exception round-tripping and will need to produce `(u64 selector, CXS body)` format instead of the old `CrossExn`.
