# Canonicalize HogDB NNInt Persistence and Hashing with XDR

**Issue:** [#1467](https://github.com/groundhogchain/hogd/issues/1467)
**Date:** 2026-04-16

## Problem

HogDB currently persists `kNNInt128` state by copying raw little-endian
`__int128` object bytes into RAMDB and reconstructing values by copying those
bytes back into `int128_t`. That format is consensus-visible because the DB
hash consumes the stored value bytes directly.

This creates two problems:

1. The persisted and hashed bytes are tied to GNU `__int128` object
   representation rather than the protocol's existing `int128_xdr` type.
2. The code path is duplicated across direct DAO writes, staged lock commits,
   and RPC decoding of raw `kNNInt128` items, making it easy to regress back to
   raw object-byte handling.

Before testnet, we are willing to make a format break and do not need backward
compatibility with databases written using the old raw-`__int128` layout.

## Design

Persisted and hashed `kNNInt128` values will use the XDR wire encoding of
`int128_xdr`.

The semantic definition of the canonical format is:

```cpp
x23::serialize(to_xdr(value))
```

That means:

- the high 64-bit half is encoded first
- both halves are encoded in XDR wire order
- the resulting bytes are exactly the bytes that should go into RAMDB and into
  the DB hash

We will not call `x23::serialize()` directly at every hot path. Instead, we
will add small stack-based helpers that are byte-for-byte equivalent to
`x23::serialize(to_xdr(value))` and the inverse decode path. These helpers are
an implementation detail for avoiding heap allocation and eliminating repeated
serialization boilerplate. They do not define a new format.

## Scope

This change applies to every path that interprets raw persisted `kNNInt128`
bytes:

- direct HogDB DAO reads and writes for contract NNInt values
- direct HogDB DAO reads and writes for address balances
- staged committed changes emitted by `NNInt128Lock` and `BalanceLock`
- RPC conversion code that reads raw `kNNInt128` bytes and exposes them as
  `int128_xdr`

The hash machinery itself does not need a new format layer. It already hashes
the exact staged or stored value bytes. Once those bytes become canonical XDR
bytes, both incremental and full DB hash computation automatically use the new
format.

## Changes by File

### 1. `hogd/base/xdr_util.h`

Add dedicated helpers for canonical `int128_t <-> 16-byte XDR wire bytes`
conversion.

Requirements:

- encode to bytes without heap allocation
- decode from a 16-byte span without going through raw `__int128` object bytes
- produce bytes exactly equal to `x23::serialize(to_xdr(value))`
- decode bytes exactly as if they had been deserialized into `int128_xdr` and
  then passed through `from_xdr(...)`

This file already defines `to_xdr(int128_t)` and `from_xdr(int128_xdr)`, so it
is the right place for the canonical conversion helpers.

### 2. `hogd/hogdb/hogdb_dao.cc`

Replace all raw `byte_view(cpu_to_le(val))` and `writable_byte_view(int128_t)`
usage for persisted `kNNInt128` state.

Affected paths:

- `read_address_balance`
- `read_nnint128`
- `write_address_balance`
- `write_nnint128`

Each write must store canonical XDR bytes. Each read must decode canonical XDR
bytes back to `int128_t`.

### 3. `hogd/hogdb/item_lock/balance_lock.cc`

Replace raw `cpu_to_le(value)` staging with canonical XDR-wire staging for
balance commits.

### 4. `hogd/hogdb/item_lock/nnint128_lock.cc`

Replace raw `cpu_to_le(value)` staging with canonical XDR-wire staging for
contract-data NNInt commits.

### 5. `hogd/hogdb/rpc_from_exec.cc`

Replace the raw copy from persisted bytes into `int128_t` with canonical XDR
decode, then expose the result through `to_xdr(...)`.

This keeps RPC behavior aligned with the stored canonical bytes and avoids
reintroducing dependence on raw `__int128` object layout when interpreting
stored state.

## Testing

Regression coverage must prove both the byte format and the consensus-visible
hash behavior.

### 1. Byte-format persistence test

Add a HogDB test that:

- writes a contract-data NNInt value whose high and low halves are both
  nonzero and distinct, so swapped-half or raw-`__int128` encodings are
  observable in the byte comparison
- inspects the raw persisted bytes
- asserts those bytes are exactly equal to `x23::serialize(to_xdr(value))`
- asserts those bytes are not equal to the old raw little-endian `__int128`
  object representation

Repeat the same check for an address balance entry.

This test guards the direct DAO persistence format.

### 2. Staged-commit path test

Add a HogDB test that commits an NNInt or balance mutation through the normal
`BlockTX` or lock-staging path and then inspects the persisted bytes after
commit.

Assertions:

- staged commits persist canonical XDR bytes
- the committed value still round-trips correctly through the public DAO read
  path

This test guards the non-DAO write path where the current bug also exists.

### 3. Hash regression test

Add a HogDB test that verifies the DB hash is driven by the canonical XDR
bytes.

The test should:

- compute the DB hash before a controlled NNInt write
- perform the write through a committed block path
- recompute the full DB hash
- independently construct the expected hash delta by hashing the exact element
  `key || type || x23::serialize(to_xdr(value))`, using the same LtHash helper
  code used by HogDB
- verify that the stored bytes and resulting DB hash behavior match the
  canonical format rather than the old raw-`__int128` bytes

The test must explicitly compare canonical XDR bytes against the old raw
little-endian `__int128` bytes and show that the canonical bytes are the ones
that drive the resulting hash behavior, not just a value round-trip.

## Non-Goals

- backward compatibility with old databases
- migration tooling
- changing unrelated storage item formats
- refactoring the general DB hash code beyond what is needed for the new
  canonical bytes to flow through existing APIs

## Result

After this change, any persisted or hashed `kNNInt128` state will be encoded in
the protocol's canonical XDR representation rather than raw `__int128` object
bytes. The DAO, lock staging, and RPC interpretation paths will all agree on
that representation, and tests will explicitly guard both the storage bytes and
the hash-visible behavior.
