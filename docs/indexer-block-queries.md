# Indexer block queries

The `IndexerBlockExplorer1` service supports two exact-height reads:

- `get_block_by_height`: the canonical committed `ConsensusValue`.
- `get_transactions_by_block`: transaction hash and height references, with bounded pagination.

The event service remains the committed-block source. The indexer stores a query copy of consensus metadata. It does not copy transaction bodies or execution logs into another block archive.

## Storage and consistency

Strict ingestion writes block metadata, transaction references, transaction rows, committed identity, and progress in one synchronous RocksDB batch. The public query boundary advances only after that write succeeds. Empty blocks have metadata even though they have no references. Transactions without emitted events still have references.

The new records use separate prefixes in the existing default column family:

| Record | Key suffix | Value |
| --- | --- | --- |
| Format | `queries:format` | Storage format version (`block-query-v1`) |
| Block metadata | `queries:blocks:` + height | XDR consensus value and transaction count |
| Transaction reference | `queries:refs:` + height + transaction hash | Zero-based ordinal in ascending hash order |

These suffixes follow `meta:indexer_durable:`. Heights and ordinals are eight-byte big-endian values. Hashes are the 32 unsigned transaction-hash bytes. Reference ordinals detect missing rows during a page walk without reading the whole block. They are index positions, not execution positions.

Each query seeks directly to the requested height or cursor key. It reads at most the page limit plus one reference for continuation. Metadata lookup verifies the stored height and canonical hash against the committed identity. Database, decode, and iterator failures remain errors.

## Coverage and genesis

This version serves heights 1 through the indexer's highest contiguous committed height. The outbox starts at height 1 and supplies only the genesis hash as its initial boundary. It does not supply the full genesis value, so height 0 is outside block-query coverage.

Coverage bounds are method-specific. Event and address history retain their genesis-inclusive lower bound of 0; block metadata and block transaction pages start at 1. Consumers must use the bounds returned by the method they call.

Before the first block is indexed, transaction pages report `IndexerQueryNotReady`. Block lookup fails with a system RPC error because `BlockLookupResult` has no unavailable arm. Once ready, block lookup returns `BlockNotFound` outside coverage. Transaction pages outside coverage are empty and include the actual oldest/latest heights.

An empty transaction page alone does not prove a block exists. Clients must also obtain its metadata and check coverage. The consensus tip can be ahead of the indexer's query boundary.

The query index does not prune history today. Any future pruning must update metadata, references, transaction records, coverage, and cursor validity together.

## Pagination

- References are sorted lexicographically by unsigned transaction-hash bytes, ascending or descending.
- A zero limit selects 50. Larger limits are capped at 200.
- An empty cursor starts a walk. A returned empty cursor ends it.
- A continuation cursor carries the height, block hash, order, and last returned transaction hash. The cursor's order takes precedence over the request field.
- A valid cursor resumes after its last reference, including after restart and while the chain advances.
- Once the block projection is ready, a matching, structurally valid continuation above its indexed tip reports `IndexerQueryHistoryUnavailable`. Retry with the same cursor after the replacement index catches up; its block identity is checked once that height is available.
- Malformed, oversized, foreign-query, wrong-height, wrong-block, or missing-position cursors report `IndexerQueryInvalidCursor`. These failures are not successful exhaustion or a promise of durable bulk-feed recovery.
- Clients must retain the cursor bytes unchanged. There is no execution-order meaning in this traversal.

## Existing databases and protocol changes

The format marker is first written during fresh strict epoch initialization. Older strict-ingestion databases have no marker and cannot prove that the new stores contain their earlier history. They remain available for existing reads and ingestion, but block queries report not-ready until a separate rebuild completes. Continued ingestion does not add the missing marker or claim complete block coverage. An incompatible marker still requires migration before opening.

The marker versions the stored layout and consensus-value encoding, independently of the wire protocol token. A protocol pin change alone does not require a rebuild. An incompatible storage or consensus-value encoding change must bump this marker and provide a verified rebuild or migration. Per-row hash checks detect corruption; they do not replace storage compatibility review.

Legacy `consume_events` databases retain their existing reopen behavior for other queries. They have no complete block projection and cannot serve the new queries.

### Initial rebuild procedure

1. Record the existing binary, protocol token, database directory, and indexed tip. Keep the original directory and binary.
2. Confirm that the authoritative event source can replay every required height from 1, with compatible schemas. If it cannot, stop and resolve the history gap. Do not claim a partial rebuild is complete.
3. Start a separate indexer using the reviewed binary, the same authoritative event endpoint, a new empty `--db-path`, and distinct listener ports. Use the existing `--event-addr`, `--block-explorer-port`, and `--metrics-port` options.
4. Wait for replay to catch up. Verify metadata hashes, page membership against known receipts, empty blocks, and reported bounds through the new listener.
5. Restart the new indexer and repeat the checks. An interrupted rebuild resumes from its own committed progress; repeated delivery is idempotent.
6. Switch the gateway to the verified listener under the normal deployment procedure. Keep the old index intact. A switch back requires the matching old binary and compatible source protocol; the old binary does not gain block-query support.

There is no automatic in-place backfill or protocol migration in this change. The stored transaction rows and block hashes cannot reconstruct missing consensus values.

## Public API coordination

Public JSON-RPC methods and their wire version are owned by thruput-tools. Block-lookup system errors remain backend-unavailable, never not-found. The internal node retains its zero-limit default; public validation owns its own limit rules.

The internal page-query outcomes distinguish invalid cursors from unavailable continuation history. Event, transaction and address walks also report `IndexerQueryInvalidCursor` for malformed cursors and `IndexerQueryHistoryUnavailable` for a saved watermark above the current serving watermark. Their version-3 cursors bind the method and event filters or root address. Event filters are normalized by sorting and removing duplicates within address/topic alternatives and across whole filters. Topic positions and filter grouping remain significant. Continuations retain the saved order, position and watermark; request order/anchor are ignored and page size may change.

Version-2 event, transaction and address cursors are rejected as invalid after upgrading the indexer. Clients must not silently restart the walk. This changes opaque continuation tokens, not public method registration or the block-cursor format. The query digest is an identity check, not authentication: resume-position membership, complete-page integrity and bounded event scanning remain in [GH-428](https://linear.app/grndhg/issue/GH-428).

Runtime and bindings must use the same protocol commit for RPC introductions. Payload-free outcomes preserve XDR result layouts but do not bypass that identity check. Legacy tools maps unrecognized outcomes to backend-unavailable; native adapters must map invalid cursors to invalid params and unavailable continuation history to history-unavailable. These internal changes do not activate the native public API.
