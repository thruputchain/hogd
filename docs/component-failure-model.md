# hogd Component Failure Model

**Status:** Pre-production contract

This document defines how logical validator components should behave when a
peer starts late, stops, crashes, or loses durable state. It is aimed at
developers and reviewers. The operator-facing response guide lives in the
[validator recovery runbook](https://github.com/groundhogchain/ops/blob/main/docs/validator-recovery.md).

The contract is intentionally stricter than process liveness. A process can be
running while its data is stale, its dependencies are unavailable, or its
recovery work is incomplete. Readiness must describe whether the component can
serve its product role safely.

## System Shape

The coordinator is adjacent to hogd and owns finalized block ordering. The
hogd services split execution and derived data into smaller failure domains:

| Component | Role | Durable state |
|---|---|---|
| Coordinator (`chuckd`) | Finalized chain order and scheduler input | Consensus and catch-up records |
| `hog-mempool` | Registration, scheduling, and transaction coordination | No authoritative chain history |
| `hog-db` | Canonical application state | HogDB data |
| `hog-event` | Event commitment, committed indexer outbox, and replay source | Events files, outbox records, and epoch origins |
| `hog-indexer` | Product query indexes derived from committed blocks | Indexed rows and one contiguous progress checkpoint |
| `hog-exec` | Transaction execution | No authoritative chain history |
| `hog-auth` | Authentication checks and subscribed auth state | Reconstructable from canonical state |

The indexer is derived state. Losing or stopping it must not change consensus
or canonical application state. `hog-event` is the durable bridge that allows
the indexer to fall behind and later catch up.

## Recovery Tiers

The supported recovery boundary is part of the deployment contract:

1. **Dependency delay:** peers can start in any order and retry registration.
2. **Process restart:** a component restarts with its existing durable data.
3. **Same-volume crash recovery:** the process was killed at any instruction,
   but data successfully synced before the crash remains available.
4. **Durable corruption:** the process fails closed and reports which state is
   unusable. It must not silently skip a block or advance a watermark.
5. **Volume or node loss:** the deployment replaces the full node from an
   accepted clean-chain or snapshot procedure. Local process recovery cannot
   recreate data that consensus commits only by hash.

The first production release begins from a clean chain. After an on-disk format
ships, a later binary must either read that format or declare and enforce a
clean-volume migration.

## Invariants

- `hog-event` does not return a block's event root until the data needed for
  same-volume close-to-commit recovery is durable.
- Event commit is the publication barrier. A closed candidate is not pullable
  by the indexer until consensus finalizes it and the event outbox commits it.
- Committed outbox records form one contiguous consensus-hash chain within and
  across supported protocol epochs.
- The indexer atomically persists a block's derived rows and its contiguous
  progress checkpoint. It never advances progress across a missing block.
- A stopped indexer may create arbitrary lag. Restarting it resumes at the
  exact next committed block and eventually reaches the event watermark.
- Product query endpoints remain unready while initial indexer catch-up is in
  progress. Consensus components do not treat indexer lag as canonical-state
  corruption.
- Startup reconciliation fails closed on contradictory durable state. It does
  not guess an origin, ignore a gap, or delete evidence to make startup pass.
- Graceful shutdown is an optimization, not a correctness dependency. The
  same-volume unclean restart path must remain correct when shutdown times out.

## Failure Boundaries

| Failure | Expected behaviour | Readiness | Required proof |
|---|---|---|---|
| Indexer starts before event | Indexer retries connection without changing durable progress | Indexer unready | `scripts/ci/startup_order_test.sh indexer-before-event` |
| Event starts before indexer | Event continues committing outbox records; no delivery is dropped | Event ready after reconciliation; indexer absent | Startup-order test plus real event replay test |
| Indexer disconnects while blocks advance | Event retains every committed block after the indexer's checkpoint; reconnect resumes at the exact next height | Indexer unready until caught up | `IndexerPullController.ReconnectsAfterEventServerTransportLoss`; `HogDevnode.MultiHeightIndexerGapReplaysThroughPull` |
| Indexer exits after applying rows but before its next request | Atomic rows and progress reopen together; replay is idempotent | Indexer unready until caught up | `StrictIndexerTest.SuccessfulStrictApplyReopensAllRowsAndProgressTogether`; `HogIndexerProcess.RecoversAfterUncleanKillAndResumesPull` |
| Event exits before close returns | Scheduler has no durable event-root acknowledgement and must not finalize that candidate | Event unavailable; block production stops | `EventRpcTest.CrashBeforeCloseLeavesNothingPullableAfterRestart` |
| Event exits after close but before commit | Restart selects the finalized artifact by consensus event root and publishes exactly that block | Event unready during reconciliation | `EventRpcTest.RecoveryPicksCanonicalEventsFileAmongCompetingBlocks`; `HogDevnode.DataDirPromotesFinalizedEventsFileAfterRestart` |
| Event exits after committed publication | Restart validates the committed chain and resumes from its durable tip | Event unready during reconciliation | `HogEventProcess.RecoversCommittedOutboxAfterUncleanKill`; committed-record reopen and pull tests |
| Temp and final event names survive a crash as hard links | Reusing a process-local block ID must create a fresh temp inode and must not truncate the final file | Event unready if integrity cannot be proven | `EventRpcTest.ReusedBlockIdWithLinkedTempDoesNotTruncatePublishedFile` |
| `hog-exec` or `hog-auth` is unavailable | Registration/readiness prevents new work from using a missing dependency; committed state is unchanged | Affected execution path unready | `scripts/ci/startup_order_test.sh starve-auth`; `starve-exec`; `starve-db`; `starve-event`; `Mempool.RuntimeStaysUnreadyWhenExecutorNeverRegisters` |
| `hog-db` or coordinator restarts on intact data | Canonical state and finalized ordering reopen without rollback; block production pauses until dependencies reconcile | Validator unready until canonical heights agree | `HogDevnode.DataDirRecoversAfterUncleanKill`; data reopen and coordinator catch-up tests |
| Protocol or storage format is unsupported | Startup reports the mismatch and refuses unsafe replay | Affected component unready | Real historical-format fixture or clean-volume migration test |
| Durable file is missing, oversized, malformed, or root-inconsistent | Reader rejects it before partial publication or unbounded allocation | Affected component unready | Missing, truncation, size-bound, XDR, and root-mismatch tests |

## Start And Stop Behaviour

Services should tolerate dependency start order through explicit registration
and retry. Starting a listener is not enough to mark a component ready; it must
first reconcile its durable state and required peers.

The preferred graceful stop order is:

1. stop admitting new scheduler work;
2. let the current block settle through event and database commit;
3. stop event, database, execution, and authentication services; and
4. stop the coordinator.

The indexer can stop at any point because its source is the committed event
outbox. A deployment may stop the whole runtime as one restart domain, but the
component crash boundaries above must still hold.

## Performance Boundary

Durability does not require rebuilding the full payload on every lifecycle
step. Keep these costs separate:

- During execution, stream each event/body pair once into the block's events
  file and build commitment metadata incrementally.
- At close, wait for pending writes, sync the one file, finish the event root,
  and publish the content-addressed name without copying the payload.
- On a normal live commit, use metadata retained by the live block context and
  publish a small committed record.
- On restart promotion, pay the cold-path cost to read and validate the
  finalized file because the live context no longer exists.
- During indexer catch-up, validate and apply exactly the next record. Do not
  make event startup rehash every retained payload before replay can begin.

A benchmark used to justify this path must exercise open, execution, close,
commit, and indexer ingestion. A close-only benchmark does not measure the
normal validator lifecycle.

## Operator Signals Required

Operators should not need to infer recovery from process names alone. The
runtime should expose:

- finalized coordinator height and consensus hash;
- canonical HogDB height;
- event outbox committed height and reconciliation status;
- indexer durable height, event source height, and lag;
- per-component starting, ready, degraded, and error states; and
- a diagnostic reason when a component refuses recovery.

These signals define the boundary between automatic recovery and an operator
incident. The ops runbook translates them into safe actions.
