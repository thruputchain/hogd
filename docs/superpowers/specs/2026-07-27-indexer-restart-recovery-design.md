# Indexer Restart Recovery Design

**Date:** 2026-07-27
**Status:** Implemented
**Issue:** #3149

## Problem

Before this change, the event service sent committed transaction logs and bodies
directly to each connected indexer. Delivery failures were dropped,
registrations were in memory, and durable event files omitted transaction bodies
and block-height ordering. An indexer that missed one block could ingest later
rows above a permanently stalled contiguous watermark. Point lookup could see
those rows, but paginated event and address-history APIs could not.

The contiguous watermark must not be advanced past missing data. It protects
opaque pagination cursors from rows arriving below a previously served cut.

## Decision

Add a durable, event-side indexer outbox. Closing a candidate atomically writes
the complete indexer payload before returning its event root to the scheduler.
Committing receives the canonical `ConsensusValue`, verifies the candidate, and
promotes it to one immutable record per committed block. Each record contains:

- a format version;
- the full consensus value and its hash;
- the previous committed consensus hash;
- the complete `TxEventLogBatch`; and
- the index-aligned `ProposedTransactionBatch`.

Records exist for empty blocks. They are atomically published before live
delivery by syncing a unique temporary file, linking it to the final no-clobber
name, removing the temporary name, and syncing the directory. Commit is not
acknowledged until publication and directory sync succeed. An existing
byte-identical record is idempotent, while an identity or payload conflict is
fatal.

Before either candidate or committed publication, event computes the complete
`IndexerEvent2.indexer_event_apply_committed_block` XDR record size: RPC call
header, origin, full `ConsensusValue`, every `ProposalTxs` segment and hash,
event/body vectors, block identity, and record marker. Anything above x23's
unchunked 31-bit record-body limit is rejected before a durable write.

The committed record carries a digest over its full payload because transaction
bodies are deliberately outside the consensus event root. Loading validates the
digest, batch lengths, each body's transaction hash against its paired event
log, and the recomputed event root. Published corruption is fatal; incomplete
candidate or temporary files are not part of the committed outbox.

Scheduler `BlockID` is process-local and may be regenerated during recovery. It
is used only to locate candidate state and alias commit RPCs; it is not part of
the committed outbox payload, committed header, wire value, payload digest, or
indexer identity ledger. A committed record is identified by its
`ConsensusValue` hash and full-payload digest.

Chuckd persists the consensus value before calling scheduler commit. A durable
candidate therefore closes the chain-ahead/outbox-missing window: if the process
stops after consensus persistence but before promotion, catchup must resubmit the
same `ConsensusValue` and promote the matching candidate. If promotion succeeds
before the scheduler database commit, the linked consensus identity remains
authoritative and catchup completes the database side. Startup must reconcile
both directions against recovered canonical chain identity before replay; an
orphaned or conflicting record is never delivered.

Delivery is ordered and at least once. Indexer registration reports the
receiver's outbox origin, durable contiguous watermark, and the consensus hash
and full-payload digest at that boundary. The event service rejects a boundary
identity mismatch, then replays every later record in ascending order before
admitting that receiver to live delivery.
Successful `indexer_event_apply_committed_block` completion acknowledges one
record.
Indexer application uses one synchronous RocksDB `WriteBatch` spanning the
transaction rows, address rows, per-height payload digest, and contiguous
watermark, so acknowledgement has one crash-atomic durability point. Duplicate
delivery of the same height and digest succeeds; a conflicting digest or a
height other than the exact durable next height is rejected without writes.

Outbox metadata defines its network identity, first height, and first previous
consensus hash. Indexer metadata records the same origin and the next expected
height. A fresh or truncated index cannot adopt the first available mid-chain
record as a new contiguous base. This rollout requires a clean chain so both
sides begin with the same origin.

### Protocol Epoch Policy

Each durable envelope records the `hogp/<commit>` that defines its payload.
The first committed record in each protocol epoch also publishes an immutable
per-protocol origin containing its first height and parent consensus hash.
Startup enumerates stable envelope metadata before decoding payloads. Records
from older protocol commits remain immutable archival files and are not decoded
with current generated types. They are accepted only as one ordered prefix below
an unbroken current-protocol suffix that starts at its durable epoch origin and
whose tip exactly matches the scheduler's canonical `ConsensusValue`.
An archival record after the current suffix, an unknown envelope version, a
current-protocol gap, an origin mismatch, or archival records without enough
current-protocol state to prove the boundary leaves the event service unhealthy.
The implementation does not guess an origin.

Reconciliation separately records the scheduler-admissible canonical tail. A
durable H+1 record found while the scheduler reports H remains withheld from
replay and live delivery. A later catchup commit must supply the exact retained
`ConsensusValue` before H+1 becomes admissible.

There is no legacy mid-chain migration. A recovered devnode chain without a V2
outbox origin fails closed even when the outbox directory is empty. The hosted
rollout and persistent devnode adoption both require a clean chain so event and
indexer establish height-1 V2 origins together; strict V2 never reconstructs an
origin from a later canonical tip.

Wire evolution follows the same rule. Existing RPC versions and signatures stay
unchanged; payload additions use a new RPC version with distinct procedure
identifiers so C++, Go, and other generators can retain both contracts. Protocol
schema changes are mirrored into `groundhog-go/protocol-go` and bindings are
regenerated, but `hogp.go` advances only after the protocol commit exists.

The first implementation supports the current topology of one event service and
one indexer. The indexer registers once during a bounded startup window, with
progress read from its database. Only connection failures and an event origin
that is not yet available are retried. Protocol mismatch, progress or content
conflict, corruption, replay failure, and stale generation are fatal. A later
indexer generation may advance the durable slot only while a newly started event
process performs its one startup registration. Once registration succeeds,
exact-generation retries remain idempotent; a different generation is rejected
for the process lifetime. There is no live indexer replacement workflow.

The validator node is fail-fast after startup. `SchedEvent2` prepare,
reconciliation, and activation form a one-shot startup barrier. Once activated,
the mempool and executors retain their original event endpoints for the process
lifetime; a second prepare or activation is an error. There are no per-block
event leases, executor endpoint refreshes, or live event-service replacement.
Any transport exception after a DB/event commit has been selected is ambiguous
and terminates the process after already-launched coroutines have drained.

An outbox publication coroutine lock serializes origin selection, durable
promotion, scheduler-admissible-tail advancement, ordered queue insertion, and
registration snapshots. Publication and its queue admission complete in that
critical section, so registration's final resnapshot cannot miss an unresolved
reservation or a newly deliverable record. The separate short state mutex only
protects receiver ownership and queue bookkeeping.

No state mutex is held during RPC. The installed connection has one serialized
delivery lane. New scheduler confirmations notify the lane without waiting for
network delivery, preserving the rule that indexer latency cannot block
consensus commit. A live delivery rejection or transport failure is fatal and
lets the validator supervisor restart the whole node. Registration repeatedly
snapshots and drains the scheduler-admissible tail, then resnapshots under the
publication lock and installs only when no later admissible record remains.
There is no window in which a durable or in-flight record is omitted from
admission. Shutdown closes active x23 streams and waits for the owned delivery
worker to drain.

The indexer stays in its startup state until initial replay succeeds. Missing
origins, missing records, chain conflicts, and corrupt records are fatal, never
healthy lag. The rollout requires a clean chain; a legacy or truncated chain
cannot infer a new origin from its current tip.

This work does not add a block product API. The existing block-by-height RPC
continues to return `BlockNotFound`; the indexer persists only the per-height
consensus identity needed for duplicate detection and replay fencing. Live
event-service hot replacement is likewise excluded.

Published outbox records are retained indefinitely. Consumer groups, bounded
retention, index pruning, and full rebuild after deliberate outbox deletion are
separate work.

## Recovery Invariants

1. A committed block is recoverable after the event service has acknowledged
   commit, even if no indexer was connected.
2. Event logs and transaction bodies retain their positional association.
3. Empty blocks advance the same contiguous sequence as non-empty blocks.
4. Replay never runs concurrently with live delivery to the same indexer.
5. The indexer watermark advances only in strict height order.
6. A restart after any successful delivery can safely replay that delivery.
7. Readiness is not healthy while scheduler-admissible durable outbox records
   remain unapplied.
8. Candidate publication precedes consensus persistence, and committed replay
   includes only records reconciled with canonical consensus identity.
9. Outbox and indexer origins prove that no retained prefix is missing.

## Hosted Validation

- Exercise process and persistent-volume replacement with a multi-block gap.
- Exercise a live gap with later rows already present above the watermark.
- Verify event and address pagination, point lookup, empty blocks, and lag
  metrics.
- Roll out on a clean Devnet chain after the aligned release is published.

## Rejected Shortcuts

- Advancing the watermark to the maximum observed height hides missing history.
- Persisting only `pending_heights_` cannot recover blocks never delivered.
- Replaying `.events` files loses transaction bodies and empty-block ordering.
- Resetting only the indexer restores future progress but discards history.
