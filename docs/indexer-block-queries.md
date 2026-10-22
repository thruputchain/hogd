# Indexer block queries

`IndexerBlockExplorer1` provides two exact-height reads:

| Method | Result |
| --- | --- |
| `get_block_by_height` | Canonical committed `ConsensusValue` |
| `get_transactions_by_block` | Paginated transaction hash and height references |

The event service is the authoritative committed-block source. The indexer stores consensus metadata without duplicating transaction bodies or execution logs in a block archive.

## Storage and consistency

Strict ingestion writes metadata, references, transaction rows, committed identity and progress in one synchronous RocksDB batch, then advances query coverage. Empty blocks have metadata; transactions without events still have references.

Records use the default column family, under `meta:indexer_durable:`:

| Key suffix | Value |
| --- | --- |
| `queries:format` | `block-query-v1` storage marker |
| `queries:blocks:` + height | XDR consensus value and transaction count |
| `queries:refs:` + height + transaction hash | Zero-based ordinal in ascending hash order |

Heights and ordinals are eight-byte big-endian values. Hashes are the 32 unsigned transaction-hash bytes. Ordinals detect missing references during pagination; they do not describe execution order.

Block queries seek directly to the height or cursor key and read at most the page limit plus one reference. Metadata reads verify height and canonical hash against committed identity. Database, decode and iterator failures remain errors.

## Coverage

Block queries cover heights 1 through the highest contiguous indexed block. Height 0 is excluded because the event source supplies the genesis hash, not its full consensus value. Event and address history retain a lower bound of 0; use each method's returned bounds.

| State | Block lookup | Block transaction page |
| --- | --- | --- |
| Before the first indexed block | System RPC error; no unavailable result variant exists | `IndexerQueryNotReady` |
| Outside coverage, once ready | `BlockNotFound` | Empty page with actual coverage bounds |

An empty page does not prove block existence: check metadata and coverage. The consensus tip may exceed indexed coverage.

History is not pruned. Future pruning must update records, references, metadata, coverage and cursor validity together.

## Block pagination

- Sort by unsigned transaction-hash bytes, ascending or descending.
- Limit 0 selects 50; the maximum is 200.
- Empty input cursor starts a walk; empty output cursor ends it. Preserve cursor bytes unchanged.
- Cursors bind height, block hash, order and last returned hash. Cursor order overrides request order. Resume after the last reference, including across restarts and chain growth.
- Once the block projection is ready, a matching continuation above indexed coverage returns `IndexerQueryHistoryUnavailable`. Retry the same cursor after catch-up; block identity is checked when available.
- Malformed, oversized, foreign-query, wrong-height, wrong-block or missing-position cursors return `IndexerQueryInvalidCursor`, not exhaustion. This is not a durable bulk-feed recovery guarantee.

## Database compatibility and rebuild

Fresh strict epoch initialization writes the format marker. Older strict databases without it retain existing reads and ingestion, but cannot serve block queries until rebuilt. Continued ingestion does not certify earlier coverage. Legacy `consume_events` databases also lack a complete block projection. An incompatible marker requires migration before opening.

The marker versions storage layout and consensus-value encoding separately from the protocol token. A protocol pin change alone requires no rebuild; an incompatible encoding or layout change requires a marker bump and verified migration or rebuild. Row hash checks do not establish storage compatibility.

There is no automatic backfill. Transaction rows and block hashes cannot reconstruct missing consensus values.

To rebuild:

1. Keep the old database and binary; record their protocol token and indexed tip.
2. Confirm the event source can replay every height from 1 with compatible schemas. Resolve any gap before proceeding.
3. Start a separate indexer with a fresh `--db-path`, the same `--event-addr`, and distinct `--block-explorer-port` and `--metrics-port` values.
4. After catch-up, verify metadata hashes, reference membership against known receipts, empty blocks and bounds. Restart and repeat. Interrupted replay resumes from committed progress; repeated delivery is idempotent.
5. Switch the gateway through the normal deployment procedure. Retain the old index. Switching back requires its matching binary and a compatible source protocol.

## Event and transaction history

Event, whole-chain and root-address walks use version-3 cursors. Version-2 cursors are invalid after upgrade; clients must not silently restart. The block-cursor format is separate.

- Cursors bind the method and event filters or root address. Event query identity ignores duplicate alternatives and ordering within/across filters, but preserves topic positions and filter grouping. The digest identifies a query; it does not authenticate it.
- Continuations retain order, position and watermark. Request order/anchor are ignored; page size may change. Event and root-address continuations seek directly to the saved key.
- Coverage is checked before position membership. A saved watermark above serving coverage returns `IndexerQueryHistoryUnavailable`; a missing or nonmatching position returns `IndexerQueryInvalidCursor`.
- An existing secondary reference with a missing or inconsistent transaction record fails internally. Event resume positions may name validated candidates that do not match topics.

Event/root pages fail whole on encountered iterator, key, decode, hydration or projection errors. Events are validated before topic filtering. Root and whole-chain reads verify transaction body hashes; event-only legacy reads may omit bodies.

Event pages hydrate and validate at most 1,000 new candidates, matching or not. When more remain, the cursor advances past the last validated candidate, even on an empty page. Only an empty cursor ends the walk. This budget excludes continuation membership checks and does not bound storage latency, repeated transaction-log decoding or work after cancellation.

Whole-chain queries read rows and order candidates by stored height/hash, then decode and validate the returned page and resume position. These checks cannot detect every deleted row or index entry. Block-query behavior is unchanged. Broader resource and completeness work remains in [GH-428](https://linear.app/grndhg/issue/GH-428).

## Public API boundary

Thruput-tools owns JSON-RPC methods, wire versions and public limit validation. Runtime and bindings must use the same protocol commit, including for payload-free outcome additions.

Block-lookup system errors map to backend-unavailable, never not-found. Legacy tools maps unknown outcomes to backend-unavailable. Native adapters map invalid cursors to invalid params and unavailable continuation history to history-unavailable. Internal query changes do not activate the native public API.
