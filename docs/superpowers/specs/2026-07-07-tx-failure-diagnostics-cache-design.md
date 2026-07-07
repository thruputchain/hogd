# Best-effort client-visible diagnostics for nondeterministic tx failures

Date: 2026-07-07
Status: approved design, pre-implementation

## Problem

Since #2878, hogdb reports *why* a transaction's two-phase commit failed
(`DbConflictResult`, built during `try_push_new_subtransaction`). Today those
diagnostics dead-end in a trace log in
`SystemCallHandler::log_db_conflict_diagnostic()`
(`hogd/hogexec/runtime/syscall_handler.cc`). A client whose transaction was
excluded from a proposal by a nondeterministic conflict has no way to learn
what happened: the tx never commits, never reaches the indexer, and
`get_transaction_info` returns `TxInfoNotFound`.

Goal: on a best-effort basis, let a client look up a recently failed tx by
hash and see the conflict diagnostic, for roughly 30 seconds (a few blocks)
after the failed attempt.

## Architecture (one line)

Executor detects a 2PC failure with diagnostics → fire-and-forget report on
its **existing event-server channel** (new `EventExecutor1` method) → event
server immediately relays to **every registered indexer** (new
`IndexerEvent1` method, existing connections) → indexer holds
`tx hash → diagnostic` in a short-lived in-memory cache → client's existing
`get_transaction_info` returns a new "recent failure" outcome until the
entry expires or the tx commits.

Everything is best-effort by construction: the executor and event server
drop reports rather than block or buffer; the indexer drops reports rather
than grow unboundedly; expiry is passive. No new connections, servers, or
discovery anywhere — both hops ride wiring that already exists.

### Decisions made during brainstorming

- **Cache lives in the indexer**, not the mempool or event server: the
  indexer is the existing client query surface for tx-by-hash.
- **Diagnostics route through the event server**, not executor→indexer
  direct and not via the scheduler/mempool. A direct connection was
  considered first, but it would have required introducing the indexer to
  the scheduler — and the indexer is deliberately out of consensus. The
  long-term plan is to shard the indexer (multiple machines/disks) with
  instances coming on/offline freely; they must stay decoupled from
  consensus components. The event server is already the indexer fan-out
  point: it holds the only indexer registrations in the system
  (`EventRPCContext::add_indexer`, `hogd/event/rpc_context.h` — already a
  vector of clients). This also resolves the `TODO(jhumphri)` in
  `hogd/event/rpc_from_indexer.h` ("should the indexer register with the
  mempool instead?") — answer: no, event-only.
- **Failure reports bypass the event server's block pipeline.** Committed
  event data is buffered per block (`EventBlockContext`) and forwarded to
  indexers at commit; a nondeterministically failed tx will never commit,
  so its report is relayed immediately and touches no block state. Note
  the executor never calls `eventexecutor_emit_event` for these txs
  (`executor_context.cc` gates emission on OK/deterministic/reverted), so
  a separate method is the honest shape — this is not committed data.
- **Lookup via a new `TxInfoResult` arm** on `get_transaction_info`, not a
  separate method: one client call answers "where is my tx".
- **Exactly one diagnostic per failure** on the wire and in the cache
  (single fixed-size `DbConflictDiagnostic`, not the `DbConflictResult`
  union). Release-build proposals already only produce
  `kDbDiagnosticFirst`; debug builds producing `kDbDiagnosticAll` send
  `conflicts[0]` and drop the rest. Reporting more diagnostics in local
  testing is a later, additive change.
- **All XDR lands in the protocol submodule.** An earlier draft put a new
  executor→indexer service in a hogd-internal `.x` under `hogd/xdr/`;
  with the event-server route there is no new service — just one added
  method on each of two existing protocol programs — so nothing internal
  remains to split out.
- **Attempt height** comes from the existing
  `SystemCallHandler::get_block_height()` (reads the db tip); no new
  plumbing.

## Components

### 1. Protocol changes (protocol submodule; one PR, then the groundhog-go bump flow)

- `protocol/db_conflict.x`: new fixed-size
  `struct TxFailureDiagnostic { BlockHeight attempt_height;
  DbConflictDiagnostic conflict; }` — shared by both hops and the public
  lookup arm.
- `protocol/event_rpc.x`, `EventExecutor1`: new method
  `eventexecutor_report_tx_failure(SignedTransactionHash,
  TxFailureDiagnostic)` (exact signature may add BlockID/TransactionID for
  tracing parity with `eventexecutor_emit_event`; decided in the plan).
- `protocol/indexer.x`:
  - `IndexerEvent1`: new method
    `indexer_event_report_tx_failure(SignedTransactionHash,
    TxFailureDiagnostic)` — arrives on the indexer's existing event-facing
    RPC server; no new port.
  - `TxInfoOutcome` gains `TxInfoRecentFailure = 2`; `TxInfoResult` gains
    a `TxInfoRecentFailure` arm carrying `TxFailureDiagnostic`.
  - Semantics documented in the `.x`: a committed record always wins (a tx
    that conflicted, was retried, and landed reports `TxInfoFound`); the
    failure arm appears only on a db miss with a cache hit; the payload is
    best-effort and advisory — absence proves nothing.

### 2. Executor side (`hogd/hogexec`)

- `EventServerProxy` (`hogd/hogexec/runtime/event_server_proxy.h`) gains
  `report_tx_failure(...)`; the executor already holds this connection
  from its intro (`EventCard`), so there are no config or discovery
  changes of any kind.
- At the propose-path points where `two_phase_commit_failed_` is set and
  `db_proxy_.last_push_conflict().level() != kDbDiagnosticNone` (exactly
  where `log_db_conflict_diagnostic()` sits today), send
  `(signed tx hash, attempt height, first DbConflictDiagnostic)`. Only
  proposal mode ever has a non-None level, so validation and viewing paths
  are untouched by construction. The tx hash is already computed by
  `SystemCallHandler` (#1905); the height comes from
  `get_block_height()`.
- The send is strictly fire-and-forget: detached, never awaited on the
  propose path, **not** counted in `pending_event_calls_` (block
  finish must not wait on diagnostics), errors trace-logged and dropped.

### 3. Event server side (`hogd/event`)

- The `EventExecutor1` handler (`rpc_from_exec.cc`) implements
  `eventexecutor_report_tx_failure` by relaying the report to every client
  in `EventRPCContext::indexers_`, fire-and-forget: no registered indexer →
  drop; a send error → trace-log and drop (an offline indexer shard must
  never disturb the event pipeline). No interaction with
  `EventBlockContext` or the commit flow.
- With multiple registered indexers (future sharding), broadcast is
  correct and cheap: reports are single fixed-size messages, and each
  indexer independently caches what it hears.

### 4. Indexer side (`hogd/indexer`)

- New `RecentTxFailureCache`, modeled on the mempool's
  `failed_authentication_txs_` (`hogd/mempool/rpc_from_coord.h`):
  `SynchronizedMap<SignedTransactionHash, TxFailureDiagnostic>` entries
  bucketed by expiry block height.
  - Expiry: entries are inserted under bucket
    `attempt_height + expiry_delta`; buckets at or below the indexer's
    contiguous indexed watermark are pruned when `consume_events` advances
    it. `expiry_delta` is configurable, default 5 blocks (matches
    `kFailedAuthenticationTxsBlockExpiryDelta`; ~30 s at current block
    times). Block-height expiry is deterministic and testable where
    wall-clock is not.
  - Latest attempt wins on duplicate hash (a retried tx's most recent
    conflict is the relevant one).
  - Global entry cap; new reports are dropped when full. Entries are
    fixed-size, so memory is strictly `cap × constant`.
- `IndexerEventRpc` (`rpc_from_event.h`) gains the
  `indexer_event_report_tx_failure` handler writing into the cache — same
  server, same port, same trust level as `indexer_event_process_events`.
- `IndexerBlockExplorerRpc::get_transaction_info`
  (`hogd/indexer/rpc_from_block_explorer.h`): db lookup first; on miss,
  consult the cache; on cache hit return the `TxInfoRecentFailure` arm;
  otherwise `TxInfoNotFound` as today.

## Error handling & trust

- No failure in this feature may affect execution, proposal, event
  pairing, or indexing correctness — every send/relay/store/lookup
  degrades to "no diagnostic".
- Reports travel only on existing internal channels
  (executor→event→indexer), same trust posture as event emission itself.
  Bounds (global cap, fixed-size entries) hold even against a buggy peer.

## Testing

- **Cache unit tests**: expiry exactly at watermark ≥ attempt + delta,
  latest-wins on duplicate hash, global cap drops new reports, prune on
  watermark advance.
- **Indexer RPC tests**: report → `get_transaction_info` returns the
  `TxInfoRecentFailure` arm with the right diagnostic; ingest the same tx
  committed → `TxInfoFound` wins; advance the watermark past expiry →
  `TxInfoNotFound`.
- **Event server tests**: a report relays to all registered indexers;
  with none registered it drops without error; an indexer send failure
  does not propagate to the executor-facing response.
- **Executor tests**: a failed 2PC push with diagnostics triggers exactly
  one report to a test-double event server; no report when the level is
  `kDbDiagnosticNone`; block finish (`await_event_return`) does not wait
  on in-flight diagnostic sends.

## Out of scope (deliberately)

- Reporting more than one diagnostic per failure (debug/local-testing
  option to be added later).
- Diagnostics for deterministic failures, auth failures, or validation
  failures.
- Indexer sharding itself — but the route is sharding-ready: reports fan
  out to however many indexers are registered with the event server, and
  indexer churn affects nothing outside the event server's client list.
- Cross-node propagation: only the proposing node's indexer(s) learn the
  diagnostic; a client querying another node sees `TxInfoNotFound`.
  Acceptable under best-effort semantics.
- HTTP surface: lookups remain XDR RPC via the existing gateway path.
