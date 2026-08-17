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

### Pattern Context

This design adapts the transactional outbox and store-and-forward patterns:
committed producer records form a durable ordered log, the consumer pulls from
its durable checkpoint, and the consumer atomically persists each record's
effects and checkpoint so retries are idempotent. Unlike a textbook
transactional outbox, HogDB and event durability cannot share one database
transaction. The scheduler confirmation barrier therefore controls pull
eligibility after both commits succeed, while canonical consensus identity and
startup reconciliation repair the permitted crash windows. This is not
distributed two-phase commit.

Add a durable, event-side indexer outbox. Closing a candidate atomically writes
the complete indexer payload before returning its event root to the scheduler.
Committing receives the canonical `ConsensusValue`, verifies the candidate, and
promotes it to one immutable record per committed block. Each record contains:

- a format version;
- the full consensus value and its hash;
- the previous committed consensus hash;
- the complete `TxEventLogBatch`; and
- the index-aligned `ProposedTransactionBatch`.

Records exist for empty blocks. They are atomically published before becoming
pullable by syncing a unique temporary file, linking it to the final no-clobber
name, removing the temporary name, and syncing the directory. Commit is not
acknowledged until publication and directory sync succeed. An existing
byte-identical record is idempotent, while an identity or payload conflict is
fatal.

Before either candidate or committed publication, event computes the complete
`EventIndexer1.event_indexer_pull_next` block-reply XDR record size: RPC reply
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
orphaned or conflicting record is never returned.

Pulling is ordered and at least once. Each request reports the indexer's outbox
origin, durable contiguous watermark, and the consensus hash and full-payload
digest at that boundary. The event service rejects a boundary identity mismatch
and returns only the exact next scheduler-admissible record. An uninitialized
indexer receives either the first record or the current origin when the outbox is
caught up. A request carrying progress advanced through a previous reply is its
acknowledgement; a lost reply safely causes the same immutable block to be
returned again.

Indexer application uses one synchronous RocksDB `WriteBatch` spanning the
transaction rows, address rows, per-height payload digest, and contiguous
watermark, so progress has one crash-atomic durability point. Duplicate
application of the same height and digest succeeds; a conflicting digest or a
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
from older protocol commits remain immutable archival files and are accepted
only as one ordered prefix below an unbroken current-protocol suffix that starts
at its durable epoch origin and whose tip exactly matches the scheduler's
canonical `ConsensusValue`. Pulling an archival record currently decodes it with
the current generated payload type after checking the envelope's recorded
protocol. Replay across epochs therefore requires that the committed payload XDR
remain wire-compatible; an incompatible payload change requires either a
retained versioned decoder or a clean-chain migration.
An archival record after the current suffix, an unknown envelope version, a
current-protocol gap, an origin mismatch, or archival records without enough
current-protocol state to prove the boundary leaves the event service unhealthy.
The implementation does not guess an origin.

Reconciliation separately records the scheduler-admissible canonical tail. A
durable H+1 record found while the scheduler reports H remains withheld from
pull requests. A later catchup commit must supply the exact retained
`ConsensusValue` before H+1 becomes admissible.

There is no legacy mid-chain migration. A recovered devnode chain without a
durable outbox origin fails closed even when the outbox directory is empty. The
hosted rollout and persistent devnode adoption both require a clean chain so
event and indexer establish height-1 durable origins together; strict ingestion
never reconstructs an origin from a later canonical tip.

This clean rollout folds the durable methods into the in-place V1 RPC contracts.
It is intentionally not wire-compatible with the old runtime, and no V2 aliases
or compatibility services are retained. Protocol schema changes are mirrored
into `groundhog-go/protocol-go` and bindings are regenerated, but `hogp.go`
advances only after the protocol commit exists.

The first implementation supports the current topology of one event service and
one indexer. The indexer owns one pull loop and reads progress from its database
for every request. Connection failures and an event origin that is not yet
available are retried. Each RPC has a deadline, and initial catch-up must finish
without a wall-clock completion deadline; retained replay can grow with chain
history. Protocol mismatch, progress or content conflict, corruption, and
durable apply failure are fatal. No reverse indexer listener, registration
generation, or live-delivery worker is required.

The validator node is fail-fast after startup. `SchedEvent1` prepare,
reconciliation, and activation form a startup barrier with no correctness
deadline. Once activated, the mempool and executors retain their original event
endpoints for the process lifetime. Repeating the exact activation, or preparing
the same card to reconcile a later canonical tip, is idempotent; a different
card is an error. There are no per-block event leases, endpoint refreshes, or
live event-service replacement. Any transport exception after a DB/event commit
has been selected is ambiguous and terminates the process after already-launched
coroutines have drained. Trusted commit and confirmation result checks run only
at the commit DAG join, after all sibling operations have settled.

An outbox publication coroutine lock serializes origin selection, durable
promotion, scheduler-admissible-tail advancement, and pull snapshots. A pull
copies the current origin and admissible boundary while holding that lock, then
loads the immutable next record without holding it. New scheduler confirmations
only advance the admissible tail; they perform no indexer network work, so
indexer latency cannot block consensus commit. Shutdown fails the active client
stream and joins the indexer's owned pull thread.

The indexer stays in its startup state until initial replay succeeds. After its
first caught-up reply, transport loss, `NotReady`, or known unapplied work marks
readiness as error until a later consistent caught-up reply. Missing origins,
missing records, chain conflicts, and corrupt records are fatal, never healthy
lag. The rollout requires a clean chain; a legacy or truncated chain cannot
infer a new origin from its current tip.

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
2. Event logs and transaction bodies retain their positional association and
   are serialized in canonical `ProposalTxs` hash order.
3. Empty blocks advance the same contiguous sequence as non-empty blocks.
4. One indexer pull loop applies records serially in exact height order.
5. The indexer watermark advances only in strict height order.
6. A restart after any successful apply can safely request that block again.
7. Readiness is not healthy while scheduler-admissible durable outbox records
   remain unapplied.
8. Candidate publication precedes consensus persistence, and committed replay
   includes only records reconciled with canonical consensus identity.
9. Outbox and indexer origins prove that no retained prefix is missing.

## Verification Map

The focused tests below map the recovery contract to executable coverage. Test
names are stable reviewer entry points rather than a substitute for the broader
event, indexer, and devnode suites.

| Invariant or failure mode | Primary coverage |
| --- | --- |
| Real event publication is recoverable without a connected indexer, and event/body pairs survive replay | `IndexerPullController.RealEventServerReplaysArchivedAndCurrentEpochsAndResumesAfterRestart`; `HogDevnode.MultiHeightIndexerGapReplaysThroughPull` |
| Empty blocks and protocol epochs advance the same strict sequence | `EventRpcTest.PullAdvancesThroughConsecutiveEmptyEpochs`; `StrictIndexerTest.ConsecutiveEmptyEpochsRequireAdjacentSequenceAndCanonicalBoundary` |
| A fresh indexer starts at the oldest retained origin and traverses into the current epoch | `EventRpcTest.FreshPullStartsAtOldestRetainedEpoch`; `IndexerPullController.RealEventServerReplaysArchivedAndCurrentEpochsAndResumesAfterRestart` |
| Durable progress and indexed rows reopen atomically, then resume from the exact next height | `StrictIndexerTest.SuccessfulStrictApplyReopensAllRowsAndProgressTogether`; `IndexerPullController.RealEventServerReplaysArchivedAndCurrentEpochsAndResumesAfterRestart` |
| A lost reply repeats the same immutable block and duplicate application is idempotent | `EventRpcTest.ExactProgressReturnsNextAndUnchangedProgressRepeatsIt`; `StrictIndexerTest.DuplicateLedgerAndFutureGapWriteNothing` |
| Scheduler confirmation gates pull eligibility | `EventRpcTest.CommittedRecordBecomesPullableOnlyAfterConfirmation` |
| A dropped event connection clears readiness, is retried, and live indexing resumes | `IndexerPullController.ReconnectsAfterEventServerTransportLoss` |
| A blocked pull is bounded by its RPC deadline and reconnects | `IndexerPullController.RpcTimeoutReconnectsAfterBlockedPull` |
| Initial catch-up has no wall-clock deadline, and shutdown cancels an in-flight pull before joining | `IndexerPullController.DestructionCancelsBlockedPull` |
| Event/body candidate bytes follow canonical proposal order | `EventBlockContextTest.CandidateSerializesEventBodiesInCanonicalProposalOrder`; `IndexerOutboxTest.NoncanonicalEventBodyOrderIsRejected` |
| Lost activation replies and same-card restart reconciliation are idempotent | `Mempool.EventActivationRetryAfterLostSuccessReplyIsIdempotent`; `Mempool.RegisteredEventSameCardRepreparesAtAdvancedCanonicalTip` |
| Origin, identity, corruption, and gap conflicts fail closed | `EventRpcTest.ForgedAndUnknownProgressConflict`; `EventRpcTest.ReconciliationRejectsDisconnectedArchivalPrefix`; `StrictIndexerTest.RejectsPrevProposalBodyEventRootAndDigestConflicts` |

The archived-to-current integration fixture rewrites current-format test records
under an archival protocol identity. It proves origin ordering, RPC traversal,
durable application, and restart behavior, but does not prove decoding of an
incompatible historical payload schema. That limitation is the wire-compatibility
requirement stated in the protocol epoch policy above.

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
