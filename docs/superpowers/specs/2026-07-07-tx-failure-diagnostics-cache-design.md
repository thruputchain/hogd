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

Executor detects a 2PC failure with diagnostics → fire-and-forget RPC
**direct to the indexer** (new internal `IndexerExec` service) → indexer
holds `tx hash → diagnostic` in a short-lived in-memory cache → client's
existing `get_transaction_info` returns a new "recent failure" outcome until
the entry expires or the tx commits.

Everything is best-effort by construction: the executor drops reports rather
than block the propose path; the indexer drops reports rather than grow
unboundedly; expiry is passive.

### Decisions made during brainstorming

- **Cache lives in the indexer**, not the mempool or event server: the
  indexer is the existing client query surface for tx-by-hash.
- **Executor → indexer direct connection**, not relayed via the event server
  or mempool: the event server's contract is "committed data only", and a
  dedicated producer→consumer channel is the right long-term shape. Cost:
  a bit of discovery plumbing (below) so executors learn the address.
- **Lookup via a new `TxInfoResult` arm** on `get_transaction_info`, not a
  separate method: one client call answers "where is my tx".
- **Exactly one diagnostic per failure** on the wire and in the cache
  (single fixed-size `DbConflictDiagnostic`, not the `DbConflictResult`
  union). Release-build proposals already only produce
  `kDbDiagnosticFirst`; debug builds producing `kDbDiagnosticAll` send
  `conflicts[0]` and drop the rest. Reporting more diagnostics in local
  testing is a later, additive change to the internal `.x` only.
- **Service named `IndexerExec`** (not `IndexerExecDiag`) so future
  executor→indexer needs can share it.
- **Discovery via the intro-card flow, not a config flag**: the indexer
  intros itself to the scheduler (mirroring `schedevent_intro_event`), and
  the scheduler hands the indexer's address to executors as a new optional
  field in `IntroExecResult`. Optional means nodes without an indexer — or
  executors that intro before the indexer registers — simply run with
  diagnostics off.
- **Attempt height** comes from the existing
  `SystemCallHandler::get_block_height()` (reads the db tip); no new
  plumbing.

## Components

### 1. XDR / protocol changes

**New internal file `hogd/xdr/indexer_exec.x`** (hogd-internal, following the
`event_cursor_internal.x` precedent; added to `hogd/xdr/CMakeLists.txt`):

- `struct TxFailureDiagnostic { BlockHeight attempt_height;
  DbConflictDiagnostic conflict; }` — fixed-size; types come from
  `protocol/types.x` and `protocol/db_conflict.x`.
- `program IndexerExec`, version `IndexerExec1`, method
  `indexer_exec_report_tx_failure(SignedTransactionHash,
  TxFailureDiagnostic)` returning void. Executor is the client, indexer the
  server. Never exposed on the public gateway.

**Protocol-submodule changes** (one protocol PR, then the groundhog-go bump
flow):

- `protocol/indexer.x`:
  - `TxInfoOutcome` gains `TxInfoRecentFailure = 2`.
  - `TxInfoResult` gains a `TxInfoRecentFailure` arm carrying
    `{ BlockHeight attempt_height; DbConflictDiagnostic conflict; }` (a
    struct defined in `indexer.x`; the internal `TxFailureDiagnostic` stays
    internal).
  - Semantics documented in the `.x`: a committed record always wins (a tx
    that conflicted, was retried, and landed reports `TxInfoFound`); the
    failure arm appears only on a db miss with a cache hit; the payload is
    best-effort and advisory — absence proves nothing.
- Discovery plumbing (see "Executor side" below):
  - A card type for the indexer's `IndexerExec` server address, e.g.
    `IndexerExecCard { Protocol protocol; ServerAddress address; }`.
  - `IntroExecResult` in `protocol/sched_exec.x` gains an optional
    `IndexerExecCard indexer<1>`.
  - A new indexer→scheduler intro method (mirroring
    `schedevent_intro_event(EventCard)`) so the scheduler learns the
    address; stored in the scheduler's `Registration` singleton
    (`hogd/mempool/registration.h`). Registration is optional and not
    gated: executor intro does not wait for an indexer. This connection is
    entirely new — today the indexer talks only to the event server
    (`event_indexer_intro`, `hogd/indexer/indexer.cc`) and nothing in the
    scheduler knows the indexer exists.
  - The indexer binary gains an optional `--scheduler-addr` flag (next to
    the existing `--event-addr`); omitted → no intro is sent and the
    feature is off network-wide.

### 2. Executor side (`hogd/hogexec`)

- The executor learns the indexer's `IndexerExec` address from the optional
  card in its `IntroExecResult` (the same response that already delivers db
  shards and the event server). Card absent → the whole feature is a no-op;
  validators and test rigs need zero changes.
- At the propose-path points where `two_phase_commit_failed_` is set and
  `db_proxy_.last_push_conflict().level() != kDbDiagnosticNone` (exactly
  where `log_db_conflict_diagnostic()` sits today), hand
  `(signed tx hash, attempt height, first DbConflictDiagnostic)` to a
  sender. Only proposal mode ever has a non-None level, so validation and
  viewing paths are untouched by construction.
- The sender is strictly non-blocking: bounded queue, drop-on-full,
  drop-on-disconnect, never awaited on the propose path. A slow or dead
  indexer costs nothing but dropped diagnostics.
- The executor computes/obtains the `SignedTransactionHash` of the proposed
  tx; the attempt height comes from the existing
  `SystemCallHandler::get_block_height()` (db tip).

### 3. Indexer side (`hogd/indexer`)

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
- New internal `RpcServer<...>` for `IndexerExec1` on its own port
  (the indexer already stands up two RPC servers in
  `hogd/indexer/indexer.cc`; this is a third). Same trust level as the
  event-server channel: internal infrastructure only, no client input.
- `IndexerBlockExplorerRpc::get_transaction_info`
  (`hogd/indexer/rpc_from_block_explorer.h`): db lookup first; on miss,
  consult the cache; on cache hit return the `TxInfoRecentFailure` arm;
  otherwise `TxInfoNotFound` as today.

## Error handling & trust

- No failure in this feature may affect execution, proposal, or indexing
  correctness — every send/store/lookup degrades to "no diagnostic".
- The report port is internal-only, same trust posture as
  `indexer_event_process_events`. Bounds (global cap, fixed-size entries)
  hold even against a buggy executor peer.

## Testing

- **Cache unit tests**: expiry exactly at watermark ≥ attempt + delta,
  latest-wins on duplicate hash, global cap drops new reports, prune on
  watermark advance.
- **Indexer RPC tests**: report → `get_transaction_info` returns the
  `TxInfoRecentFailure` arm with the right diagnostic; ingest the same tx
  committed → `TxInfoFound` wins; advance the watermark past expiry →
  `TxInfoNotFound`.
- **Executor tests**: a failed 2PC push with diagnostics triggers exactly
  one report to a test-double `IndexerExec` server; no report when the
  level is `kDbDiagnosticNone` or no indexer card was delivered at intro;
  the propose path completes normally with the sender's queue full.
- **Scheduler tests**: indexer intro registers the card; `IntroExecResult`
  carries it when present and an empty optional otherwise.

## Out of scope (deliberately)

- Reporting more than one diagnostic per failure (debug/local-testing
  option to be added later).
- Diagnostics for deterministic failures, auth failures, or validation
  failures.
- Cross-node propagation: only the proposing node's indexer learns the
  diagnostic; a client querying another node sees `TxInfoNotFound`.
  Acceptable under best-effort semantics.
- HTTP surface: lookups remain XDR RPC via the existing gateway path.
