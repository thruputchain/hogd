# hog_exec_hs

A Haskell implementation of `hog_exec`, the hogd transaction executor
(`hogd/hog_exec`). It is a port, not a redesign: wire protocols, gas, error
classification and contract-visible behavior follow the C++ implementation,
which remains the reference.

## Layout

| Path | Contents |
|---|---|
| `xdrgen/` | XDR compiler: x23-dialect `.x` -> Haskell |
| `gen-xdr.sh` | Regenerates `src/Hogd/Xdr*` from `protocol/` and `hogd/xdr/`, and `SyscallMeta.hs` |
| `src/X23/Xdr.hs` | XDR and CXS encodings (x23 semantics) |
| `src/X23/Rpc.hs` | ONC RPC over TCP (x23 RpcStream compatible) |
| `src/Hogd/Xdr/*` | Generated protocol types (do not edit) |
| `src/Hogd/Exec/*` | The executor; module paths mirror `hogd/hog_exec/*` |
| `src/Hogd/Exec/Runtime/SyscallMeta.hs` | Generated syscall table (`gen_syscalls.py --syscall_meta_hs`; do not edit) |
| `app/` | The `hog-exec-hs` executable (`hog_exec.cc`) |
| `src/Hogd/Crypto*` | The `hogd/crypto` layer the executor uses (SHA-256, SipHash, Ed25519, ML-DSA-65) |
| `cbits/` | mldsa-native (vendored from `external/mldsa-native`) and its two hogd builds |
| `src/Hogd/BuiltContracts/*` | The contract traits the precompiles implement (what `built_contracts/generate_header.py` generates in C++), written out from the interface crates' sources |
| `test/` | hspec test suite |
| `test/Harness/` | In-memory hog_db (`FakeDb`) and event server (`FakeEventServer`) served over ONC RPC, a transaction-driving environment (`Env`) and test contracts (`NativeContracts`) |

Module names follow the C++ file they port:
`hogd/hog_exec/runtime/rv32/machine.cc` -> `Hogd.Exec.Runtime.Rv32.Machine`.
Code that the C++ executor takes from elsewhere in hogd (`hogd/base`,
`hogd/crypto`, `hogd/storage_cost`, ...) lives under `Hogd.Base`,
`Hogd.Crypto`, `Hogd.StorageCost`, and so on.

## Generated XDR representation

`xdrgen` maps XDR to Haskell as follows (see `xdrgen/Main.hs`):

* **struct** -> record with the XDR field names. Modules use
  `DuplicateRecordFields`, `NoFieldSelectors` and `OverloadedRecordDot`, so
  read fields with `x.field` and build values with record syntax. A field
  named after a Haskell keyword gets a trailing underscore (`type_`).
* **enum** -> `newtype E = E Int32` with a pattern synonym per tag. Enums are
  open, as in x23: decoding accepts any value. Tags are capitalized
  (`kRPCNone` -> `KRPCNone`). x23 enums are scoped, so a tag name used by
  several enums (or clashing with a type) is qualified: `HostFnErrorCode_Abandon`.
  `<enum>_values` lists the declared tags.
* **union** -> one constructor per arm, named `<Union>_<arm field>`, or
  `<Union>_<case label>` / `<Union>_default` for void arms. An arm covering
  several case labels, or the default arm, also carries the discriminant as its
  first field. `discriminant_<Union>` returns the discriminant.
* **typedef** -> type synonym plus `put_T`, `get_T`, `cxsPut_T`, `cxsGet_T`.
  Structs, unions, enums and primitives use the `XdrType` class instead
  (`xdrPut`, `xdrGet`, `cxsPut`, `cxsGet`; `xdrEncode`, `xdrDecode`,
  `cxsEncode`).
* `opaque`/`string` -> `ByteString`; `T<n>`/`T[n]` -> `[T]`; `T *` -> `Maybe T`.
* Lower-case type names gain an `X_` prefix (`int128_xdr` -> `X_int128_xdr`);
  anonymous nested types are hoisted as `<Parent>_<field>_t`.
* **const** -> `name :: Num a => a`.
* **program** -> per version a record of handlers (`<Version>`), a
  dispatcher `<version>_service :: <Version> -> RpcService`, and client stubs
  `call_<proc> :: RpcClient -> args -> IO result`.

## Precompiles

`Hogd.Exec.Precompiles.Context.makeHogExecContext` multiplexes the native
precompiles (native token, default wallet) with the RV32 runtime.  The RV32
context is supplied through `makeHogExecContextWith (Just rv32Context)`;
`makeHogExecContext` currently passes `Nothing`, so RV32 contracts have no
runtime until that hook is wired.

## Testing against an in-memory ledger

`Harness.Env.withTestEnv` starts a `FakeDb` and a `FakeEventServer` on
localhost, connects the executor's own `DatabaseProxy` and
`EventServerProxy` to them, seeds the genesis prerequisites (native token
code and deployment, default wallet code) and opens block 1.  Seed state with
`deployContract` / `writeItem` / `grantNativeTokens`, run transactions with
`runTx` (root invoke, commit, event emission), `advanceBlock` to make commits
visible, and inspect with `lookupItem` / `nativeBalance` / `lookupDeployment`.
See the module headers for what the fake database does and does not model.

## Building

```sh
cabal build all
cabal test
```

GHC 9.4 and cabal 3.8 or newer.
