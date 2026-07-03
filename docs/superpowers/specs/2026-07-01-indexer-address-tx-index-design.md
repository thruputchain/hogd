# Indexer API rework: fold root-address tx history into the address index

**Date**: 2026-07-01
**Status**: Design — approved, pending spec review
**Related**: GitHub issue #2842 (Transaction Filter Misses Root Invoked Address)

## Problem

`IndexDB::get_transactions(TransactionQuery)` accepts an optional `address`
filter. Three things are wrong with it:

1. **It is broken (#2842).** The filter matches `query.address` only against
   `record.events.events[*].contract_address` (emitted-event producers). A
   transaction whose root invocation targets address `A` but emits no event
   from `A` is stored, returned by hash, and returned by unfiltered pagination,
   yet is silently omitted from `get_transactions(address=A)`. The stored
   `ProposedTransaction` body — which carries `invocation.invoked_address` — is
   ignored.

2. **It has no index behind it.** `get_transactions` iterates the *entire*
   `tx_index` column family (`SeekToFirst()`→`Next()`), deserializes every row,
   filters by block height and address in memory, collects all matches into a
   `std::vector`, sorts them, then slices `limit`. `tx_index` is keyed by
   `tx_hash` only, so it cannot seek by block or address. Every call — even
   `limit=10` — is O(total txs) time and memory. The `anchor`/`cursor`/`limit`
   machinery is cosmetic pagination over a full scan.

3. **Its semantics were never defined.** `protocol/indexer.x` documents
   `TransactionQuery.address` with an explicit open TODO:
   *"'involving' = source/sender only, or any address touched during
   execution?"* The filter shipped in the first pagination-RPC commit
   (a56e8f2c, Mark Mayo, 2026-06-22) with under-specified meaning.

### Account model context

In Groundhog the root `invocation.invoked_address` **is the user's account
contract** (the account is itself the invoked contract; replay/nonce
uniqueness is scoped per invoked address — see `transaction.x` `Uniquifier`).
Therefore "all transactions whose root invoked address is `A`" is exactly
**`A`'s wallet transaction history** — the single most-run query in any wallet
or block-explorer product. This is not a niche feature and is not YAGNI-able.

Ethereum base nodes deliberately omit address→tx history and push it to
off-chain indexers (Etherscan, Alchemy); `eth_getLogs` covers only
event-emitting activity and misses eventless transactions — the same gap as
#2842, at ecosystem scale. Groundhog already ships a first-party indexer
(`hogd/indexer`, the subsystem edited here), so it should serve this query
natively rather than recreate that centralization dependency. Cosmos
(`tx_search`) and Solana (`getSignaturesForAddress`) do exactly this.

## Goals

- Remove the broken, unindexed address filter from `get_transactions`.
- Provide a properly indexed `get_transactions_by_address(addr)` (wallet
  history) — O(matching rows), not O(total txs).
- Keep by-hash lookup as its own API (already exists).
- Close #2842 with no residual broken filter.

## Non-goals (explicitly deferred)

- Fixing the whole-chain `get_transactions` full-scan-and-sort. After the
  address filter is removed it remains an unindexed scan over the hash-keyed
  `tx_index`. A block-ordered tx index is the "better storage format" follow-up,
  tracked as its own issue.
- Indexing by auth-claim source address (`kSourceAddressAuthData`) or by any
  address merely touched during sub-invocation. Only the root invoked address
  is indexed.

## Design

### RPC surface

| RPC | Backing | Returns |
|-----|---------|---------|
| `get_transaction_info(hash)` | point-get on `tx_index` (keyed by hash) — **already exists** in `rpc_from_block_explorer.h` | `TxInfoResult` |
| `get_transactions(cursor)` | whole-chain walk, **no address param** | `TransactionPage` (+ cursor) |
| `get_transactions_by_address(addr, cursor)` | prefix-scan the folded address index, `kind=ROOT_TX` | `TransactionRefPage`: `{tx_hash, height}` refs (+ cursor) |

By-hash is already implemented as `get_transaction_info`. An optional rename to
`get_transaction_by_hash` is cosmetic and out of scope unless trivially free
during protocol edits.

### Fold the root-address rows into the existing address index

Rename the `contract_index` column family concept to `address_index` (it now
indexes address activity, not just contract event emission) and add a 1-byte
record-kind discriminator immediately after the address:

```
address(32) ‖ kind(1) ‖ block_height(8 BE) ‖ tx_hash(32) ‖ seq(4)   value: empty
    kind = EVENT   (0x00): one row per emitted event, keyed by the emitter's
                           contract_address — today's behavior, unchanged.
    kind = ROOT_TX (0x01): one row per transaction, keyed by
                           body.signed_tx.tx.invocation.invoked_address, seq = 0.
```

The discriminator sits **before** `block_height` so each `(address, kind)`
forms a contiguous, block-ordered key range. Index values stay empty; all
hydration reads `tx_index`, exactly as `get_events` does today.

- `get_events(A)` prefix-scans `A ‖ 0x00` — same semantics, longer prefix.
- `get_transactions_by_address(A)` prefix-scans `A ‖ 0x01`, hydrating each hit
  via a `tx_index` lookup and returning `TxInfo`.

### Return shape: lightweight refs (Solana style)

`get_transactions_by_address` returns a `TransactionRefPage` — a list of
`{tx_hash, height}` refs plus a cursor — **not** hydrated `TxInfo`. The client
hydrates on demand via the existing `get_transaction_info(hash)`. This mirrors
Solana's `getSignaturesForAddress` (signatures + slot + cursor, hydrate via
`getTransaction`).

Consequences:

- The scan needs **zero** `tx_index` hydration: the `address_index` key already
  carries `block_height` and `tx_hash`, so the walk reads refs straight off the
  index keys. Cheaper than `get_events`, which must hydrate because there is no
  standalone "get event by id" API — transactions *do* have a by-hash API, so
  refs + hydrate is the natural split here. The asymmetry with `get_events` is
  intentional and justified by that difference.
- Index rows stay empty-valued; no body storage in the address index.
- A ref may momentarily outlive its `tx_index` row only under retention/pruning
  (not yet implemented); `get_transaction_info` already returns `TxInfoNotFound`
  in that case, so the client degrades gracefully.

## Affected code

- `protocol/indexer.x`
  - Delete `address` from `TransactionQuery` (and its TODO comment).
  - Add `get_transactions_by_address`: a query struct carrying `address` +
    anchor/order/cursor/limit (mirroring `TransactionQuery` minus the removed
    field), returning a new `TransactionRefPage { TransactionRef refs<>;
    opaque cursor<>; BlockHeight latest_block; BlockHeight oldest_block; }`
    where `TransactionRef { SHA256Hash tx_hash; BlockHeight height; }`.
- `hogd/indexer/key_format.h`
  - Rename `kContractIndexColumnFamily` → `kAddressIndexColumnFamily`
    (`"address_index"`).
  - `kContractKeySize` → `+1` byte; add a `RecordKind` enum (`EVENT=0`,
    `ROOT_TX=1`).
  - `make_contract_key` / `make_contract_key_prefix` / `parse_contract_key`
    gain a `kind` parameter (rename toward `make_address_key`, etc.).
- `hogd/indexer/db.cc`
  - `create_contract_index_sst_chunk`: emit `EVENT` rows as today **and** one
    `ROOT_TX` row per transaction keyed by the body's `invoked_address`. This
    function must therefore also receive the `bodies_chunk` (the tx-index chunk
    creator already takes it); wire it through the ingest path.
  - `ContractIter` / `get_events`: seek/prefix on `address ‖ 0x00`.
  - New `IndexDB::get_transactions_by_address`: mirror `get_events`'s
    ContractIter merge-walk and cursor handling with `kind=0x01`, but emit
    `{tx_hash, height}` refs read directly off the index keys — **no** per-hit
    `tx_index` hydration.
  - `get_transactions`: delete the `if (query.address)` block.
- `hogd/indexer/rpc_from_block_explorer.h`
  - Add the `get_transactions_by_address` handler.

## Migration

None. There is no deployment and no backwards-compatibility requirement: the
`address_index` key format and CF rename can change freely, `TransactionQuery.
address` can be removed outright, and no existing indexer DB must be preserved
or upgraded in place.

## #2842 resolution

Removing `TransactionQuery.address` deletes the broken filter entirely, so the
omission cannot occur. The replacement `get_transactions_by_address` is
correct by construction: a `ROOT_TX` row is written for every transaction keyed
by its root invoked address, independent of whether it emitted events.

## Testing

- **Regression for #2842**: ingest an eventless root invocation targeting `A`
  via `indexer_event_process_events`; assert `get_transactions_by_address(A)`
  returns a ref with that `tx_hash`, and that `get_transaction_info(tx_hash)`
  then hydrates the full body (the exact case the old filter dropped). Reuse the
  PoC from the issue, retargeted to the new RPC + hydration step.
- **Event index unchanged**: existing `get_events` tests must pass as-is
  (semantics preserved; only the key prefix grew).
- **Separation**: a tx that emits an event from contract `B` while rooted at
  `A` appears in `get_transactions_by_address(A)` and in `get_events(B)`, and
  `get_transactions_by_address(B)` does **not** return it (B is an emitter, not
  the root).
- **Pagination/cursor**: multi-page `get_transactions_by_address` walks in
  block order with a stable cursor, mirroring `get_events` cursor tests.
- **by-hash + unfiltered** paths continue to return the transaction.
