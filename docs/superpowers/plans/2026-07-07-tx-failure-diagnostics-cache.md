# Tx-Failure Diagnostics Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Best-effort client-visible diagnostics for nondeterministic tx failures: executor → event server → indexer cache, surfaced via a new `TxInfoRecentFailure` arm on `get_transaction_info`.

**Architecture:** hogdb already builds `DbConflictResult` on failed 2PC pushes (#2878) but the executor only trace-logs it. This plan forwards a single fixed-size diagnostic per failed tx on the executor's *existing* event-server channel (new `EventExecutor1` method), has the event server relay it immediately to *every registered indexer* (new `IndexerEvent1` method, existing connections, no block-pipeline interaction), and caches it in the indexer in a new block-height-expiring `RecentTxFailureCache` consulted by `get_transaction_info` on a db miss. Spec: `docs/superpowers/specs/2026-07-07-tx-failure-diagnostics-cache-design.md`.

**Tech Stack:** C++23, x23 XDR/RPC (codegen from `.x` files at build time), gtest, CMake.

## Global Constraints

- **Branches:** hogd work goes on the existing `geoff/tx-failure-diag-cache` branch. The `protocol/` submodule gets its own branch `geoff/tx-failure-diagnostics` (git writes are hook-blocked outside `geoff/*` branches).
- **Build:** `cmake --build ./build -j16` from the repo root (`/Users/geofframseyer/groundhog/agent3/hogd`). XDR codegen runs as part of the build.
- **Format:** before each commit run `clang-format -i` on every touched `.h`/`.cc` file (CI uses clang-format-21; never hand-write brace/else placement). Do NOT format `.x` files (they carry `// clang-format off`).
- **Best-effort invariants (from the spec):** the executor send never blocks or fails the propose path; the event server relay never errors toward the executor and one dead indexer never blocks the others; the cache never grows past its caps; a committed record always beats a cached failure.
- **Exactly one diagnostic per failure** on the wire and in the cache: a single fixed-size `DbConflictDiagnostic`, never the `DbConflictResult` union.
- **Post-merge follow-up (not a task here):** the `protocol/` change must land on protocol `main` first, then run the `bump-groundhog-go` skill to re-sync `groundhog-go`. Pushing/PRs are the user's job.

---

### Task 1: XDR wire types + concept-satisfying stubs

Adding a method to an x23 `program` breaks every implementer's `is_<Service>` static_assert at compile time. This task adds the wire format and minimal accept-and-drop stubs in **all four** implementers so the tree builds green; later tasks replace stubs with real behavior.

**Files:**
- Modify: `protocol/db_conflict.x` (submodule — own branch/commit)
- Modify: `protocol/event_rpc.x`
- Modify: `protocol/indexer.x`
- Modify: `hogd/event/rpc_from_exec.h`, `hogd/event/rpc_from_exec.cc` (EventExecRpc)
- Modify: `hogd/hogexec/runtime/test_util/test_event_aggregator.h` (TestEventAggregatorExecRpc)
- Modify: `hogd/indexer/rpc_from_event.h` (IndexerEventRpc)
- Modify: `hogd/event/event_block_context_test.cc` (CapturingIndexerRpc)

**Interfaces:**
- Produces (used by every later task):
  - `struct TxFailureDiagnostic { BlockHeight attempt_height; DbConflictDiagnostic conflict; }` (generated into `db_conflict.hh`)
  - `RPCError eventexecutor_report_tx_failure(SignedTransactionHash, TxFailureDiagnostic)` on `EventExecutor1` (method 33)
  - `void indexer_event_report_tx_failure(SignedTransactionHash, TxFailureDiagnostic)` on `IndexerEvent1` (method 1)
  - `TxInfoOutcome::TxInfoRecentFailure = 2` and a `TxFailureDiagnostic recent_failure` arm on `TxInfoResult`

- [ ] **Step 1: Create the protocol submodule branch**

```bash
cd /Users/geofframseyer/groundhog/agent3/hogd/protocol
git checkout -b geoff/tx-failure-diagnostics
```

- [ ] **Step 2: Add `TxFailureDiagnostic` to `protocol/db_conflict.x`**

Append before the closing `} // namespace hogd`:

```c
/// Best-effort diagnostic for a transaction that failed nondeterministically
/// during two-phase commit at proposal time: the executing block height of
/// the failed attempt plus the first conflicting storage op hogdb reported.
/// Fixed-size by construction; the wire and caches carry exactly one
/// diagnostic per failure to bound memory end to end.
struct TxFailureDiagnostic {
  BlockHeight attempt_height;
  DbConflictDiagnostic conflict;
};
```

(`BlockHeight` comes from `types.hh`, already `%#include`d at the top of the file.)

- [ ] **Step 3: Add the executor→event method to `protocol/event_rpc.x`**

Add `%#include "db_conflict.hh"` next to the existing `%#include` lines at the top. Then inside `version EventExecutor1`, after `eventexecutor_close_block(...) = 32;`:

```c
    // Best-effort report that a transaction failed nondeterministically
    // during 2-phase commit and was excluded from the proposal. The event
    // server relays this immediately to every registered indexer; it touches
    // no per-block state (the tx will never commit, so it has no place in
    // the commit pipeline). The executor sends fire-and-forget and ignores
    // the result.
    RPCError eventexecutor_report_tx_failure(SignedTransactionHash,
                                             TxFailureDiagnostic) = 33;
```

- [ ] **Step 4: Extend `protocol/indexer.x`**

Add `%#include "db_conflict.hh"` next to the existing `%#include` lines. Replace the `TxInfoOutcome` enum and `TxInfoResult` union with:

```c
enum TxInfoOutcome {
  TxInfoFound = 0,
  TxInfoNotFound = 1,
  /// No committed record, but the transaction failed nondeterministically
  /// within the last few blocks and the indexer still holds the diagnostic.
  /// Best-effort and advisory: reports are dropped freely under load, only
  /// the proposing node's indexers hear them, and a later retry of the same
  /// tx may still commit (at which point TxInfoFound wins). Absence of this
  /// outcome proves nothing.
  TxInfoRecentFailure = 2
};

union TxInfoResult switch (TxInfoOutcome outcome) {
case TxInfoFound:
  TxInfo info;
case TxInfoRecentFailure:
  TxFailureDiagnostic recent_failure;
default:
  void;
};
```

Inside `version IndexerEvent1`, after `indexer_event_process_events(...) = 0;`:

```c
    // Best-effort relay (from the event server) of a transaction that failed
    // nondeterministically at proposal time. The indexer caches it briefly so
    // get_transaction_info can explain the failure; it may drop reports
    // freely (cache full) and expires them a few blocks later.
    void indexer_event_report_tx_failure(SignedTransactionHash,
                                         TxFailureDiagnostic) = 1;
```

- [ ] **Step 5: Build to see the concept static_asserts fail**

```bash
cd /Users/geofframseyer/groundhog/agent3/hogd && cmake --build ./build -j16
```

Expected: FAIL — `static_assert(is_EventExecutor1<...>)` in `hogd/event/rpc_from_exec.h` and `test_event_aggregator.h`, and `static_assert(is_IndexerEvent1<...>)` in `hogd/indexer/rpc_from_event.h` and `event_block_context_test.cc`. (This is the "failing test" for this task.)

- [ ] **Step 6: Stub all four implementers**

`hogd/event/rpc_from_exec.h` — add `#include "db_conflict.hh"` to the includes; declare after `eventexecutor_emit_event`:

```cpp
  x23::Async<RPCError>
  eventexecutor_report_tx_failure(SignedTransactionHash tx_hash,
                                  TxFailureDiagnostic diag);
```

`hogd/event/rpc_from_exec.cc` — add `#include "db_conflict.hh"`; append:

```cpp
x23::Async<RPCError>
EventExecRpc::eventexecutor_report_tx_failure(
    SignedTransactionHash /*tx_hash*/, TxFailureDiagnostic /*diag*/)
{
  // Relay to registered indexers lands in a later commit; accept and drop.
  co_return RPCError::kRPCNone;
}
```

`hogd/hogexec/runtime/test_util/test_event_aggregator.h` — add `#include "db_conflict.hh"`; in `TestEventAggregatorExecRpc`, after `eventexecutor_emit_event`:

```cpp
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  x23::Async<RPCError>
  eventexecutor_report_tx_failure(SignedTransactionHash const & /*tx_hash*/,
                                  TxFailureDiagnostic const & /*diag*/)
  {
    // Recording lands with the executor forwarding work; accept and drop.
    co_return RPCError::kRPCNone;
  }
```

`hogd/indexer/rpc_from_event.h` — add `#include "db_conflict.hh"`; in `IndexerEventRpc`, after `indexer_event_process_events`:

```cpp
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash & /*tx_hash*/,
                                  const TxFailureDiagnostic & /*diag*/)
  {
    // Cache lands in a later commit; accept and drop.
    co_return;
  }
```

`hogd/event/event_block_context_test.cc` — add `#include "db_conflict.hh"`; in `CapturingIndexerRpc` (line ~574), after `indexer_event_process_events`:

```cpp
  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    received_failure_reports.emplace_back(tx_hash, diag);
    co_return;
  }
```

and a member next to `received_bodies`:

```cpp
  std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>
      received_failure_reports;
```

- [ ] **Step 7: Build green**

```bash
cmake --build ./build -j16
```

Expected: PASS.

- [ ] **Step 8: Format and commit (protocol first, then hogd)**

```bash
clang-format -i hogd/event/rpc_from_exec.h hogd/event/rpc_from_exec.cc \
  hogd/hogexec/runtime/test_util/test_event_aggregator.h \
  hogd/indexer/rpc_from_event.h hogd/event/event_block_context_test.cc
cd protocol
git add db_conflict.x event_rpc.x indexer.x
git commit -m "indexer/event_rpc: wire format for best-effort tx-failure diagnostics"
cd ..
git add protocol hogd/event/rpc_from_exec.h hogd/event/rpc_from_exec.cc \
  hogd/hogexec/runtime/test_util/test_event_aggregator.h \
  hogd/indexer/rpc_from_event.h hogd/event/event_block_context_test.cc
git commit -m "protocol/event/indexer: tx-failure diagnostic wire format + handler stubs"
```

---

### Task 2: RecentTxFailureCache

A standalone, single-mutex, block-height-expiring map, modeled on the mempool's `failed_authentication_txs_` (`hogd/mempool/rpc_from_coord.h:618-660`) but self-contained and strictly memory-bounded.

**Files:**
- Create: `hogd/indexer/recent_tx_failure_cache.h`
- Create: `hogd/indexer/recent_tx_failure_cache_test.cc`
- Modify: `hogd/indexer/CMakeLists.txt` (add test source to the existing `indexer_test` executable, line ~16)

**Interfaces:**
- Produces (used by Task 3):
  - `RecentTxFailureCache(BlockHeight expiry_delta = kDefaultExpiryDelta, size_t max_entries = kDefaultMaxEntries)`
  - `void report(const SignedTransactionHash &, const TxFailureDiagnostic &)`
  - `std::optional<TxFailureDiagnostic> lookup(const SignedTransactionHash &) const`
  - `void prune(BlockHeight watermark)`
  - `size_t size() const`
  - Constants: `kDefaultExpiryDelta = 5` (matches `kFailedAuthenticationTxsBlockExpiryDelta`; ~30 s at current block times), `kDefaultMaxEntries = 1 << 16`

- [ ] **Step 1: Write the failing tests**

Create `hogd/indexer/recent_tx_failure_cache_test.cc`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>

#include "db_conflict.hh"
#include "hogd/indexer/recent_tx_failure_cache.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

SignedTransactionHash
make_hash(uint8_t fill)
{
  SignedTransactionHash hash;
  std::ranges::fill(hash, fill);
  return hash;
}

TxFailureDiagnostic
make_diag(BlockHeight attempt_height, DbConflictCode code)
{
  TxFailureDiagnostic diag;
  diag.attempt_height = attempt_height;
  diag.conflict.code = code;
  return diag;
}

} // namespace

TEST(RecentTxFailureCacheTest, ReportThenLookup)
{
  RecentTxFailureCache cache;
  const auto hash = make_hash(0x01);
  cache.report(hash,
               make_diag(100, DbConflictCode::kDbConflictValueMismatch));

  auto found = cache.lookup(hash);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->attempt_height, 100);
  EXPECT_EQ(found->conflict.code, DbConflictCode::kDbConflictValueMismatch);
  EXPECT_FALSE(cache.lookup(make_hash(0x02)).has_value());
}

TEST(RecentTxFailureCacheTest, LatestAttemptWins)
{
  RecentTxFailureCache cache;
  const auto hash = make_hash(0x01);
  cache.report(hash, make_diag(10, DbConflictCode::kDbConflictValueMismatch));
  cache.report(hash, make_diag(12, DbConflictCode::kDbConflictOverflow));
  // A stale (lower-height) report never downgrades the entry.
  cache.report(hash,
               make_diag(9, DbConflictCode::kDbConflictExclusiveLock));

  auto found = cache.lookup(hash);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->attempt_height, 12);
  EXPECT_EQ(found->conflict.code, DbConflictCode::kDbConflictOverflow);
  EXPECT_EQ(cache.size(), 1);
}

TEST(RecentTxFailureCacheTest, ExpiresExactlyAtDelta)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5);
  const auto hash = make_hash(0x01);
  cache.report(hash,
               make_diag(100, DbConflictCode::kDbConflictValueMismatch));

  cache.prune(104);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(105); // 100 + 5 <= 105 -> expired
  EXPECT_FALSE(cache.lookup(hash).has_value());
  EXPECT_EQ(cache.size(), 0);
}

TEST(RecentTxFailureCacheTest, ReReportExtendsLifetime)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5);
  const auto hash = make_hash(0x01);
  cache.report(hash,
               make_diag(100, DbConflictCode::kDbConflictValueMismatch));
  cache.report(hash,
               make_diag(102, DbConflictCode::kDbConflictValueMismatch));

  // The height-100 expiry row fires at 105, but the entry now belongs to
  // attempt 102 and must survive until 107.
  cache.prune(105);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(106);
  EXPECT_TRUE(cache.lookup(hash).has_value());
  cache.prune(107);
  EXPECT_FALSE(cache.lookup(hash).has_value());
}

TEST(RecentTxFailureCacheTest, CapDropsNewReportsButUpdatesExisting)
{
  RecentTxFailureCache cache(/*expiry_delta=*/5, /*max_entries=*/2);
  cache.report(make_hash(0x01),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch));
  cache.report(make_hash(0x02),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch));
  // Full: a report for a NEW hash is dropped...
  cache.report(make_hash(0x03),
               make_diag(100, DbConflictCode::kDbConflictValueMismatch));
  EXPECT_EQ(cache.size(), 2);
  EXPECT_FALSE(cache.lookup(make_hash(0x03)).has_value());
  // ...but a re-report of an existing hash still updates it.
  cache.report(make_hash(0x01),
               make_diag(101, DbConflictCode::kDbConflictOverflow));
  auto found = cache.lookup(make_hash(0x01));
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->attempt_height, 101);
}

TEST(RecentTxFailureCacheTest, PruneOnEmptyCacheIsNoop)
{
  RecentTxFailureCache cache;
  cache.prune(1000);
  EXPECT_EQ(cache.size(), 0);
}

} // namespace hogd
```

- [ ] **Step 2: Register the test source and verify it fails to build**

In `hogd/indexer/CMakeLists.txt`, add `recent_tx_failure_cache_test.cc` to the `add_executable(indexer_test ...)` source list (after `indexer_test.cc`). Then:

```bash
cmake --build ./build -j16 --target indexer_test
```

Expected: FAIL — `hogd/indexer/recent_tx_failure_cache.h` not found.

- [ ] **Step 3: Implement the cache**

Create `hogd/indexer/recent_tx_failure_cache.h`:

```cpp
#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "db_conflict.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "transaction.hh"

namespace hogd {

// Short-lived, best-effort cache of recently observed nondeterministic
// transaction failures: tx hash -> TxFailureDiagnostic (attempt height plus
// the first conflicting op).  Fed by the event server's failure-report relay
// (indexer_event_report_tx_failure); consulted by get_transaction_info when
// a tx has no committed record.
//
// Expiry is block-height based, driven by the indexer's contiguous indexed
// watermark: an entry whose latest attempt height is H is dropped once
// prune(watermark) sees watermark >= H + expiry_delta.  Wall-clock plays no
// part, so behavior is deterministic under test.  Modeled on the mempool's
// failed_authentication_txs_ (rpc_from_coord.h).
//
// Memory is strictly bounded: at most [max_entries] fixed-size entries
// (reports for new hashes are dropped when full -- best-effort), and at most
// one expiry row per (hash, attempt height) pair in the live window.
class RecentTxFailureCache {
public:
  static constexpr BlockHeight kDefaultExpiryDelta = 5;
  static constexpr size_t kDefaultMaxEntries = 1 << 16;

  explicit RecentTxFailureCache(BlockHeight expiry_delta = kDefaultExpiryDelta,
                                size_t max_entries = kDefaultMaxEntries)
    : expiry_delta_(expiry_delta), max_entries_(max_entries)
  {}

  // Best-effort insert.  The latest attempt (highest attempt_height) wins
  // for a given hash; a report for a new hash is dropped when the cache is
  // full.
  void report(const SignedTransactionHash &tx_hash,
              const TxFailureDiagnostic &diag)
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    auto it = entries_.find(tx_hash);
    if (it == entries_.end()) {
      if (entries_.size() >= max_entries_) {
        return;
      }
      entries_.emplace(tx_hash, diag);
    }
    else if (diag.attempt_height > it->second.attempt_height) {
      it->second = diag;
    }
    else if (diag.attempt_height == it->second.attempt_height) {
      // Same expiry bucket already references this hash; refresh the payload
      // without growing the expiry index.
      it->second = diag;
      return;
    }
    else {
      return;
    }
    expiry_index_[diag.attempt_height + expiry_delta_].push_back(tx_hash);
  }

  std::optional<TxFailureDiagnostic>
  lookup(const SignedTransactionHash &tx_hash) const
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    auto it = entries_.find(tx_hash);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // Drops every entry whose latest attempt height is at least [expiry_delta]
  // blocks behind [watermark].  Called by the ingest path as the indexed
  // watermark advances.
  void prune(BlockHeight watermark)
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    while (!expiry_index_.empty() &&
           expiry_index_.begin()->first <= watermark) {
      for (const SignedTransactionHash &tx_hash :
           expiry_index_.begin()->second) {
        auto it = entries_.find(tx_hash);
        // Only erase if this expiry row covers the entry's current attempt;
        // a re-report at a later height owns a later row.
        if (it != entries_.end() &&
            it->second.attempt_height + expiry_delta_ <= watermark) {
          entries_.erase(it);
        }
      }
      expiry_index_.erase(expiry_index_.begin());
    }
  }

  size_t size() const
  {
    const std::lock_guard<std::mutex> guard(mtx_);
    return entries_.size();
  }

private:
  const BlockHeight expiry_delta_;
  const size_t max_entries_;

  mutable std::mutex mtx_;
  std::unordered_map<SignedTransactionHash, TxFailureDiagnostic,
                     ByteArrayHasher>
      entries_;
  // expiry height -> hashes whose entry (may) die at that height.
  std::map<BlockHeight, std::vector<SignedTransactionHash>> expiry_index_;
};

} // namespace hogd
```

(`ByteArrayHasher` lives in `hogd/base/xdr_util.h:380`; the mempool already uses it to key `SignedTransactionHash` maps.)

- [ ] **Step 4: Build and run the tests**

```bash
cmake --build ./build -j16 --target indexer_test
./build/hogd/indexer/indexer_test --gtest_filter='RecentTxFailureCacheTest.*'
```

Expected: PASS (6 tests).

- [ ] **Step 5: Format and commit**

```bash
clang-format -i hogd/indexer/recent_tx_failure_cache.h \
  hogd/indexer/recent_tx_failure_cache_test.cc
git add hogd/indexer/recent_tx_failure_cache.h \
  hogd/indexer/recent_tx_failure_cache_test.cc hogd/indexer/CMakeLists.txt
git commit -m "indexer: block-height-expiring RecentTxFailureCache"
```

---

### Task 3: Indexer wiring — report handler, prune hook, lookup arm

**Files:**
- Modify: `hogd/indexer/rpc_from_event.h`
- Modify: `hogd/indexer/rpc_from_block_explorer.h`
- Modify: `hogd/indexer/indexer.cc`
- Test: `hogd/indexer/indexer_test.cc`

**Interfaces:**
- Consumes: `RecentTxFailureCache` (Task 2), stub `indexer_event_report_tx_failure` (Task 1)
- Produces: `IndexerEventRpc(IndexDB &, RecentTxFailureCache &)` and `IndexerBlockExplorerRpc(IndexDB &, RecentTxFailureCache &)` constructors (anyone constructing these RPC handlers must pass the cache); `get_transaction_info` returning the `TxInfoRecentFailure` arm on a db miss + cache hit

- [ ] **Step 1: Write the failing integration tests**

In `hogd/indexer/indexer_test.cc`:

Add to the includes: `#include "db_conflict.hh"` and `#include "hogd/indexer/recent_tx_failure_cache.h"`.

In the `IndexerTest` fixture: add a public accessor and member, and update the two RPC constructions in `SetUp()`:

```cpp
  RecentTxFailureCache &failure_cache() { return failure_cache_; }
```

```cpp
    event_ = std::make_unique<IndexerEventRpc>(*db_, failure_cache_);
    block_explorer_ =
        std::make_unique<IndexerBlockExplorerRpc>(*db_, failure_cache_);
```

```cpp
  RecentTxFailureCache failure_cache_;
```

Add a file-scope helper next to `create_tx_bodies` (inside the existing anonymous namespace):

```cpp
TxFailureDiagnostic
make_failure_diag(BlockHeight attempt_height)
{
  TxFailureDiagnostic diag;
  diag.attempt_height = attempt_height;
  diag.conflict.code = DbConflictCode::kDbConflictValueMismatch;
  return diag;
}
```

Add three tests at the end of the file (before `} // namespace hogd`):

```cpp
TEST_F(IndexerTest, RecentFailureVisibleThenCommittedRecordWins)
{
  TxEventLogBatch events = create_event_logs();
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());
  const SignedTransactionHash tx_hash = events[0].proposed_tx_hash;

  // Report a failure for a tx with no committed record.
  event_->indexer_event_report_tx_failure(tx_hash, make_failure_diag(100))();

  TxInfoResult result = block_explorer_->get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoRecentFailure);
  EXPECT_EQ(result.recent_failure().attempt_height, 100);
  EXPECT_EQ(result.recent_failure().conflict.code,
            DbConflictCode::kDbConflictValueMismatch);

  // The retried tx commits at height 101: the committed record wins.
  event_->indexer_event_process_events(101, events, bodies)();
  result = block_explorer_->get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result.info().height, 101);
}

TEST_F(IndexerTest, RecentFailureExpiresAsWatermarkAdvances)
{
  SignedTransactionHash failed_hash;
  std::ranges::fill(failed_hash, 0x7F);
  event_->indexer_event_report_tx_failure(failed_hash,
                                          make_failure_diag(100))();

  // Ingest committed blocks 100..105; each ingest prunes against the new
  // watermark. Give each block one distinct committed tx.
  for (BlockHeight h = 100; h <= 105; h++) {
    TxEventLog log;
    std::ranges::fill(log.proposed_tx_hash, static_cast<uint8_t>(h));
    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = h;
    event_->indexer_event_process_events(h, TxEventLogBatch{log},
                                         ProposedTransactionBatch{body})();
    TxInfoResult result =
        block_explorer_->get_transaction_info(failed_hash)();
    if (h < 105) {
      // attempt 100 + delta 5 > watermark h -> still visible.
      EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoRecentFailure)
          << "watermark " << h;
    }
    else {
      EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
    }
  }
}

TEST_F(IndexerTest, UnknownTxStillNotFound)
{
  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x03);
  TxInfoResult result = block_explorer_->get_transaction_info(tx_hash)();
  EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
}
```

- [ ] **Step 2: Build to verify the tests fail**

```bash
cmake --build ./build -j16 --target indexer_test
```

Expected: FAIL — the `IndexerEventRpc`/`IndexerBlockExplorerRpc` constructors don't take a cache yet.

- [ ] **Step 3: Wire the cache through the indexer**

`hogd/indexer/rpc_from_event.h` — add `#include "hogd/indexer/recent_tx_failure_cache.h"`; change the constructor and members:

```cpp
  IndexerEventRpc(IndexDB &db, RecentTxFailureCache &failure_cache)
    : db_(db), failure_cache_(failure_cache)
  {}
```

```cpp
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
```

In `indexer_event_process_events`, after the `CHECK(db_.consume_events(...))` line and before the `co_await Health::...` line:

```cpp
    // Expire failure diagnostics as the indexed watermark advances.
    failure_cache_.prune(db_.latest_block());
```

Replace the Task 1 stub with the real handler (drop the NOLINT line — it is no longer static):

```cpp
  // Best-effort diagnostic (relayed by the event server) for a tx that
  // failed nondeterministically at proposal time. Cached briefly so
  // get_transaction_info can explain the failure.
  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    // Careful! Both parameters are references; copy them before any
    // co_await is introduced ahead of their use.
    failure_cache_.report(tx_hash, diag);
    co_return;
  }
```

`hogd/indexer/rpc_from_block_explorer.h` — add `#include "hogd/indexer/recent_tx_failure_cache.h"`; change the constructor and members:

```cpp
  IndexerBlockExplorerRpc(IndexDB &db, RecentTxFailureCache &failure_cache)
    : db_(db), failure_cache_(failure_cache)
  {}
```

```cpp
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
```

In `get_transaction_info`, replace the miss path:

```cpp
    const IndexerStatus<IndexDB::TxRecord> status = db_.get_tx_record(tx_hash);
    if (!status) {
      // No committed record: fall back to the short-lived failure cache so a
      // client can learn why a recently rejected tx never landed. Committed
      // records always win (checked first) -- a conflicted-then-retried tx
      // reports TxInfoFound.
      if (auto diag = failure_cache_.lookup(tx_hash)) {
        TxInfoResult ret(TxInfoOutcome::TxInfoRecentFailure);
        ret.recent_failure() = *diag;
        co_return ret;
      }
      co_return TxInfoResult(TxInfoOutcome::TxInfoNotFound);
    }
```

`hogd/indexer/indexer.cc` — add `#include "hogd/indexer/recent_tx_failure_cache.h"`; in `indexer_init`, before the `RpcServer<IndexerEventRpc> event(db);` line, create the cache and pass it to both servers:

```cpp
  RecentTxFailureCache failure_cache;

  // Create the RPC server that the event server talks to.
  RpcServer<IndexerEventRpc> event(db, failure_cache);
```

```cpp
  RpcServer<IndexerBlockExplorerRpc> block_explorer(db, failure_cache);
```

- [ ] **Step 4: Build and run**

```bash
cmake --build ./build -j16 --target indexer_test indexer_exec
./build/hogd/indexer/indexer_test --gtest_filter='IndexerTest.*:RecentTxFailureCacheTest.*'
```

Expected: PASS, including all pre-existing `IndexerTest` cases.

- [ ] **Step 5: Format and commit**

```bash
clang-format -i hogd/indexer/rpc_from_event.h \
  hogd/indexer/rpc_from_block_explorer.h hogd/indexer/indexer.cc \
  hogd/indexer/indexer_test.cc
git add hogd/indexer/rpc_from_event.h hogd/indexer/rpc_from_block_explorer.h \
  hogd/indexer/indexer.cc hogd/indexer/indexer_test.cc
git commit -m "indexer: cache tx-failure reports, surface TxInfoRecentFailure on lookup"
```

---

### Task 4: Event server relay

**Files:**
- Modify: `hogd/event/rpc_context.h`, `hogd/event/rpc_context.cc`
- Modify: `hogd/event/rpc_from_exec.cc` (replace Task 1 stub)
- Test: `hogd/event/event_rpc_test.cc`

**Interfaces:**
- Consumes: wire method + `CapturingIndexerRpc`-style fakes (Task 1)
- Produces: `x23::Async<void> EventRPCContext::report_tx_failure_to_indexers(SignedTransactionHash, TxFailureDiagnostic)` (used by the `EventExecRpc` handler)

- [ ] **Step 1: Write the failing tests**

In `hogd/event/event_rpc_test.cc`, add includes (mirroring `event_block_context_test.cc`'s indexer harness): `#include "db_conflict.hh"`, `#include "indexer.hh"`, `#include <sys/socket.h>`, `#include <algorithm>`, `#include <utility>`, `#include <vector>`, and `#include "hogd/base/coropoll.h"` if not already present. Then add near the top of the file's namespace:

```cpp
namespace {

// Minimal IndexerEvent1 fake capturing failure reports (the relay under
// test); process_events is unused here.
struct FailureCapturingIndexerRpc {
  using rpc_service = IndexerEvent1;

  x23::Async<void>
  indexer_event_process_events(BlockHeight /*block_height*/,
                               const TxEventLogBatch & /*tx_events*/,
                               const ProposedTransactionBatch & /*tx_bodies*/)
  {
    co_return;
  }

  x23::Async<void>
  indexer_event_report_tx_failure(const SignedTransactionHash &tx_hash,
                                  const TxFailureDiagnostic &diag)
  {
    reports.emplace_back(tx_hash, diag);
    co_return;
  }

  std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>> reports;
};
static_assert(is_IndexerEvent1<FailureCapturingIndexerRpc *, x23::Async>);

indexer_event_client_t
make_socketpair_indexer_client(FailureCapturingIndexerRpc *server)
{
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  x23::rpc_stream_logged(default_coropoll(), fds[1], -1)->server(server);
  auto client_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  return client_stream->client<IndexerEvent1>();
}

} // namespace

TEST_F(EventRpcTest, TxFailureReportRelaysToAllIndexers)
{
  EventRPCContext ctx(temp_dir());

  FailureCapturingIndexerRpc indexer_a;
  FailureCapturingIndexerRpc indexer_b;
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_a))();
  ctx.add_indexer(make_socketpair_indexer_client(&indexer_b))();

  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x5A);
  TxFailureDiagnostic diag;
  diag.attempt_height = 42;
  diag.conflict.code = DbConflictCode::kDbConflictReplayProtection;

  ctx.report_tx_failure_to_indexers(tx_hash, diag)();

  for (const auto *indexer : {&indexer_a, &indexer_b}) {
    ASSERT_EQ(indexer->reports.size(), 1);
    EXPECT_EQ(indexer->reports[0].first, tx_hash);
    EXPECT_EQ(indexer->reports[0].second.attempt_height, 42);
    EXPECT_EQ(indexer->reports[0].second.conflict.code,
              DbConflictCode::kDbConflictReplayProtection);
  }
}

TEST_F(EventRpcTest, TxFailureReportWithNoIndexersIsNoop)
{
  EventRPCContext ctx(temp_dir());
  const SignedTransactionHash tx_hash{};
  const TxFailureDiagnostic diag{};
  // Must complete without error or hang.
  ctx.report_tx_failure_to_indexers(tx_hash, diag)();
}

TEST_F(EventRpcTest, DeadIndexerDoesNotBlockRelayToOthers)
{
  EventRPCContext ctx(temp_dir());

  // First "indexer": a client whose peer is closed immediately, so the send
  // to it fails at the RPC layer.
  int fds[2];
  CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  CHECK_EQ(close(fds[1]), 0);
  auto dead_stream = x23::rpc_stream_logged(default_coropoll(), fds[0], -1);
  ctx.add_indexer(dead_stream->client<IndexerEvent1>())();

  // Second indexer: live and capturing.
  FailureCapturingIndexerRpc live_indexer;
  ctx.add_indexer(make_socketpair_indexer_client(&live_indexer))();

  SignedTransactionHash tx_hash;
  std::ranges::fill(tx_hash, 0x11);
  TxFailureDiagnostic diag;
  diag.attempt_height = 7;
  diag.conflict.code = DbConflictCode::kDbConflictExclusiveLock;

  // Must complete (dead indexer's error swallowed) and still reach the
  // live indexer.
  ctx.report_tx_failure_to_indexers(tx_hash, diag)();

  ASSERT_EQ(live_indexer.reports.size(), 1);
  EXPECT_EQ(live_indexer.reports[0].first, tx_hash);
  EXPECT_EQ(live_indexer.reports[0].second.attempt_height, 7);
}
```

(`unistd.h` provides `close`; add the include if the file lacks it.)

- [ ] **Step 2: Build to verify failure**

```bash
cmake --build ./build -j16 --target event_test
```

Expected: FAIL — `report_tx_failure_to_indexers` is not a member of `EventRPCContext`.

- [ ] **Step 3: Implement the relay**

`hogd/event/rpc_context.h` — add includes `#include "db_conflict.hh"` and `#include "transaction.hh"`; declare after `add_indexer`:

```cpp
  // Best-effort relay of a nondeterministic tx-failure diagnostic to every
  // registered indexer. Bypasses the per-block commit pipeline entirely --
  // the tx will never commit. One offline indexer's send error is logged and
  // dropped so it never blocks the other indexers or errors toward the
  // executor.
  x23::Async<void>
  report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                TxFailureDiagnostic diag);
```

(Parameters deliberately by value: the coroutine may suspend, so it must own copies.)

`hogd/event/rpc_context.cc` — append:

```cpp
x23::Async<void>
EventRPCContext::report_tx_failure_to_indexers(SignedTransactionHash tx_hash,
                                               TxFailureDiagnostic diag)
{
  x23::CoroGuard cg;
  co_await cg(indexers_lock_);
  for (indexer_event_client_t &indexer : indexers_) {
    try {
      co_await indexer.indexer_event_report_tx_failure(tx_hash, diag);
    } catch (std::exception const &e) {
      // Best-effort: an offline indexer must not disturb the relay to the
      // remaining indexers (or the executor-facing response).
      warn("dropping tx-failure report to an indexer: {}", e.what());
    }
  }
  co_await cg.reset_and_return();
}
```

`hogd/event/rpc_from_exec.cc` — replace the Task 1 stub body:

```cpp
x23::Async<RPCError>
EventExecRpc::eventexecutor_report_tx_failure(SignedTransactionHash tx_hash,
                                              TxFailureDiagnostic diag)
{
  co_await ctx_.report_tx_failure_to_indexers(std::move(tx_hash),
                                              std::move(diag));
  co_return RPCError::kRPCNone;
}
```

- [ ] **Step 4: Build and run**

```bash
cmake --build ./build -j16 --target event_test
./build/hogd/event/event_test --gtest_filter='EventRpcTest.*'
```

Expected: PASS (the two new tests plus all pre-existing `EventRpcTest` cases). If the event test binary has a different path, find it with `find build -name 'event_test' -type f`.

- [ ] **Step 5: Format and commit**

```bash
clang-format -i hogd/event/rpc_context.h hogd/event/rpc_context.cc \
  hogd/event/rpc_from_exec.cc hogd/event/event_rpc_test.cc
git add hogd/event/rpc_context.h hogd/event/rpc_context.cc \
  hogd/event/rpc_from_exec.cc hogd/event/event_rpc_test.cc
git commit -m "event: relay tx-failure reports to all registered indexers"
```

---

### Task 5: Executor forwarding

The executor sends the report fire-and-forget at the exact points where `two_phase_commit_failed_` is set. `TestEventAggregator` learns to record reports so a new, correctly-parameterized runtime test can observe them end to end (note: `NNIntAddTest` never forwards `GetParam()` to the framework, so it silently runs the mock db for both instantiations — do not extend it; the new test file forwards the param properly).

**Files:**
- Modify: `hogd/hogexec/runtime/event_server_proxy.h`, `hogd/hogexec/runtime/event_server_proxy.cc`
- Modify: `hogd/hogexec/runtime/syscall_handler.h`, `hogd/hogexec/runtime/syscall_handler.cc`
- Modify: `hogd/hogexec/runtime/test_util/test_event_aggregator.h`, `hogd/hogexec/runtime/test_util/test_event_aggregator.cc`
- Modify: `hogd/hogexec/runtime/native_contract_test_framework.h` (accessors)
- Create: `hogd/hogexec/runtime/tx_failure_report_test.cc`
- Modify: `hogd/hogexec/runtime/CMakeLists.txt` (add the new file to the `runtime_test` source list, next to `nnint_add_test.cc` at line ~101)

**Interfaces:**
- Consumes: relay handler (Task 4), `TxFailureDiagnostic` (Task 1), existing `SystemCallHandler::get_block_height()` (`syscall_handler.h:367`, executing height = db tip + 1), existing `get_source_tx_hash()` (`syscall_handler.h:261`)
- Produces:
  - `void EventServerProxy::report_tx_failure(SignedTransactionHash, TxFailureDiagnostic)` — fire-and-forget, never throws to caller; destructor drains in-flight reports
  - `void PerTransactionEventServerProxy::report_tx_failure(SignedTransactionHash, TxFailureDiagnostic) const`
  - `SystemCallHandler::report_db_conflict_diagnostic()` (renamed from `log_db_conflict_diagnostic()`)
  - `TestEventAggregator::report_tx_failure(...)`, `wait_for_failure_reports(int64_t)`, `get_failure_reports()`; delegators of the latter two on `EventServerWrapper` and `NativeContractTestFramework`

- [ ] **Step 1: Write the failing end-to-end test**

Create `hogd/hogexec/runtime/tx_failure_report_test.cc` (the conflict recipe is copied from `NNIntAddTest.SetAddWithAdd`, which deterministically produces `ExecResultTxNonDeterministicError` in proposal mode):

```cpp
#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <utility>

#include "db_conflict.hh"
#include "exec_result.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/native_syscall_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// Verifies the executor forwards a best-effort TxFailureDiagnostic to the
// event server when (and only when) hogdb attaches conflict diagnostics to a
// failed 2-phase-commit push. Parameterized over the db test double: real
// hogdb produces diagnostics for proposal blocks; the mock produces none, so
// no report may be sent.
class TxFailureReportTest : public ::testing::TestWithParam<TestDB>,
                            public NativeContractTestFramework {
public:
  TxFailureReportTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal, GetParam())
  {}
};

TEST_P(TxFailureReportTest, NonDeterministicFailureForwardsOneReport)
{
  // Same conflict recipe as NNIntAddTest.SetAddWithAdd: method 0 set_adds
  // (0, 200); method 1 adds -100. After the first block commits the item at
  // 100, re-running method 0 then method 1 in the next block makes the
  // method-1 push fail 2pc nondeterministically.
  auto make_context = [&]() -> std::unique_ptr<RuntimeContext> {
    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, // set_add on (0, 200)
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key = make_storage_key("test");
                  int128_t set_value = 0;
                  auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
                      key, &set_value, 200,
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!r.has_value() ||
                      *r != SyscallErrorCode::kSyscallErrorCodeNone) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
    fns.emplace(1, // add on (-100)
                [](NativeSyscallContext &ctx) noexcept -> InvokeStatus<void> {
                  auto key = make_storage_key("test");
                  auto r = ctx.syscall<SyscallNo::kNonnegativeIntSetAdd>(
                      key, static_cast<int128_t const *>(nullptr), -100,
                      std::to_underlying(StorageDeltaFlags::None));
                  if (!r.has_value() ||
                      *r != SyscallErrorCode::kSyscallErrorCodeNone) {
                    return InvokeError{InvokeErrorCode::Deterministic};
                  }
                  return {};
                });
    auto native_contract = std::make_unique<NativeContract>(std::move(fns));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::move(native_contract));
    return std::make_unique<NativeContext>(std::move(contract_map));
  };

  auto context = make_context();
  auto contract_0 = make_native_contract(0);
  write_contract(contract_0);
  auto contract_addr = make_contract_address(0);
  write_deployment(contract_addr, contract_0);
  setup_grant_storage_token(contract_addr, 1'000'000'000);

  auto run_method = [&](Runtime::MethodID method) {
    return make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = contract_addr,
                                   .entrypoint =
                                       entrypoint_from_method_id(method),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                }},
        *context);
  };

  // Block 1: both methods commit (item ends at 100).
  auto handler1 = run_method(0);
  ASSERT_TRUE(handler1.root_invoke().is_root_success());
  ASSERT_EQ(handler1.try_commit_transaction(), ExecResult::ExecResultOK);
  auto handler2 = run_method(1);
  ASSERT_TRUE(handler2.root_invoke().is_root_success());
  ASSERT_EQ(handler2.try_commit_transaction(), ExecResult::ExecResultOK);
  advance_block();

  // Block 2: set_add's base (0) no longer matches the stored 100, so the
  // second method's push fails the 2-phase commit.
  auto handler3 = run_method(0);
  ASSERT_TRUE(handler3.root_invoke().is_root_success());
  ASSERT_EQ(handler3.try_commit_transaction(), ExecResult::ExecResultOK);
  auto handler4 = run_method(1);
  ASSERT_TRUE(handler4.root_invoke().is_root_success());
  ASSERT_EQ(handler4.try_commit_transaction(),
            ExecResult::ExecResultTxNonDeterministicError);

  if (GetParam() == TestDB::kHogDB) {
    // hogdb attached diagnostics; exactly one best-effort report must reach
    // the event server, carrying this tx's hash and the executing height.
    wait_for_failure_reports(1)();
    auto reports = get_failure_reports()();
    ASSERT_EQ(reports.size(), 1);
    EXPECT_EQ(reports[0].first, handler4.get_source_tx_hash());
    EXPECT_GE(reports[0].second.attempt_height, 1);
  }
  else {
    // The mock db test double produces no diagnostics (level None), so the
    // executor must not send anything. No send is ever initiated in this
    // case (the level check is synchronous), so this read does not race.
    EXPECT_EQ(get_failure_reports()().size(), 0);
  }
}

INSTANTIATE_TEST_SUITE_P(TxFailureReportTest, TxFailureReportTest,
                         ::testing::Values(TestDB::kMockInMemory,
                                           TestDB::kHogDB),
                         test_db_to_string);

} // namespace hogd
```

Add `tx_failure_report_test.cc` to the `runtime_test` source list in `hogd/hogexec/runtime/CMakeLists.txt` (next to `nnint_add_test.cc`).

- [ ] **Step 2: Build to verify failure**

```bash
cmake --build ./build -j16 --target runtime_test
```

Expected: FAIL — `wait_for_failure_reports` / `get_failure_reports` don't exist yet.

- [ ] **Step 3: Teach TestEventAggregator to record reports**

`hogd/hogexec/runtime/test_util/test_event_aggregator.h` — add `#include <utility>` and `#include <vector>` if missing; declare in the public section of `TestEventAggregator`:

```cpp
  // Records a forwarded tx-failure diagnostic (executor -> event server).
  x23::Async<RPCError> report_tx_failure(SignedTransactionHash tx_hash,
                                         TxFailureDiagnostic diag);

  // Blocks until at least [n] failure reports have arrived.
  x23::Async<void> wait_for_failure_reports(int64_t n);

  x23::Async<
      std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>>
  get_failure_reports();
```

and in the private section:

```cpp
  x23::CoroFutex<int64_t> failure_report_count_{0};
  std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>
      failure_reports_;
```

Replace the Task 1 stub in `TestEventAggregatorExecRpc` with a forwarder:

```cpp
  x23::Async<RPCError>
  eventexecutor_report_tx_failure(SignedTransactionHash tx_hash,
                                  TxFailureDiagnostic diag)
  {
    return event_aggregator_.report_tx_failure(std::move(tx_hash),
                                               std::move(diag));
  }
```

`hogd/hogexec/runtime/test_util/test_event_aggregator.cc` — append (mirror the existing `lock_`/futex usage in this file):

```cpp
x23::Async<RPCError>
TestEventAggregator::report_tx_failure(SignedTransactionHash tx_hash,
                                       TxFailureDiagnostic diag)
{
  {
    x23::CoroGuard cg;
    co_await cg(lock_);
    failure_reports_.emplace_back(std::move(tx_hash), diag);
    co_await cg.reset_and_return();
  }
  failure_report_count_.rmw([](int64_t &count) { count++; });
  failure_report_count_.broadcast(&default_threadpool());
  co_return RPCError::kRPCNone;
}

x23::Async<void>
TestEventAggregator::wait_for_failure_reports(int64_t n)
{
  while (true) {
    const int64_t current = failure_report_count_.load();
    if (current >= n) {
      co_return;
    }
    co_await failure_report_count_.wait(current);
  }
}

x23::Async<std::vector<std::pair<SignedTransactionHash, TxFailureDiagnostic>>>
TestEventAggregator::get_failure_reports()
{
  x23::CoroGuard cg;
  co_await cg(lock_);
  auto copy = failure_reports_;
  co_await cg.reset_and_return();
  co_return copy;
}
```

`hogd/hogexec/runtime/native_contract_test_framework.h` — add public delegators on `EventServerWrapper` (next to `block_emit_events`):

```cpp
  x23::Async<void> wait_for_failure_reports(int64_t n)
  {
    return event_server_.wait_for_failure_reports(n);
  }

  auto get_failure_reports() { return event_server_.get_failure_reports(); }
```

and on `NativeContractTestFramework` (next to its other public event helpers, e.g. `get_event_exec_port` at line ~582):

```cpp
  x23::Async<void> wait_for_failure_reports(int64_t n)
  {
    return event_server_wrapper_.wait_for_failure_reports(n);
  }

  auto get_failure_reports()
  {
    return event_server_wrapper_.get_failure_reports();
  }
```

- [ ] **Step 4: Implement the executor send**

`hogd/hogexec/runtime/event_server_proxy.h` — add `#include "db_conflict.hh"` and `#include <atomic>`; in `EventServerProxy`, after `emit_event`:

```cpp
  // Fire-and-forget, best-effort report that [tx_hash] failed
  // nondeterministically during 2pc, with the diagnostic hogdb attached.
  // Returns immediately; a slow or dead event server costs nothing but the
  // dropped report (errors are logged and swallowed -- diagnostics must
  // never disturb execution). In-flight sends are capped at
  // kMaxPendingFailureReports; excess reports are dropped, so a conflict
  // storm against a slow event server cannot grow memory. The destructor
  // drains in-flight sends (bounded) so a detached coroutine never touches
  // a destroyed client_.
  void report_tx_failure(SignedTransactionHash tx_hash,
                         TxFailureDiagnostic diag);

  ~EventServerProxy();
```

and private members:

```cpp
  // Soft bound on concurrently in-flight failure reports (racy over-admission
  // by a few is fine); reports past the cap are dropped, keeping the
  // fire-and-forget path memory-bounded even with an unresponsive server.
  static constexpr ssize_t kMaxPendingFailureReports = 1024;
  std::atomic<ssize_t> pending_failure_reports_ = 0;
```

Note: the atomic member (and the user-declared destructor) delete EventServerProxy's implicit moves. `make_from_exec_result` still compiles — it returns a prvalue, so C++17 guaranteed elision applies — but if the build surfaces any caller that genuinely moves an `EventServerProxy`, construct it in place there instead.

`hogd/hogexec/runtime/event_server_proxy.cc` — add `#include "db_conflict.hh"` and `#include <chrono>`, `#include <thread>`, `#include <utility>`; append:

```cpp
EventServerProxy::~EventServerProxy()
{
  // Drain detached report coroutines (cf. ~EventRPCContext) so none touches
  // a destroyed client_. Bounded: on the shutdown path with an unreachable
  // event server a retrying send may never resolve; a leaked coroutine at
  // process exit beats a shutdown hang.
  for (int i = 0;
       i < 2000 &&
       pending_failure_reports_.load(std::memory_order_acquire) > 0;
       i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (pending_failure_reports_.load(std::memory_order_acquire) > 0) {
    warn("EventServerProxy destroyed with tx-failure reports still in flight");
  }
}

void
EventServerProxy::report_tx_failure(SignedTransactionHash tx_hash,
                                    TxFailureDiagnostic diag)
{
  // Drop rather than queue unboundedly when the event server is slow.
  if (pending_failure_reports_.load(std::memory_order_acquire) >=
      kMaxPendingFailureReports) {
    return;
  }
  pending_failure_reports_.fetch_add(1, std::memory_order_release);
  [](EventServerProxy &self, SignedTransactionHash hash,
     TxFailureDiagnostic d) -> x23::Detached {
    try {
      RPCError r = co_await self.client_.eventexecutor_report_tx_failure(hash,
                                                                         d);
      if (r != RPCError::kRPCNone) {
        warn("tx-failure report dropped by event server: {}", r);
      }
    } catch (x23::XdrException const &e) {
      // Best-effort: an RPC-layer throw (#2857 containment pattern, cf.
      // emit_event above) must never escape a detached coroutine.
      warn("tx-failure report send failed: {}", e.what());
    }
    self.pending_failure_reports_.fetch_sub(1, std::memory_order_release);
    co_return;
  }(*this, std::move(tx_hash), std::move(diag));
}
```

Add to `PerTransactionEventServerProxy` in `event_server_proxy.h` (after `emit_events`; const because it only forwards through the proxy reference, and the caller `report_db_conflict_diagnostic` is const):

```cpp
  // Fire-and-forget; see EventServerProxy::report_tx_failure.
  void report_tx_failure(SignedTransactionHash tx_hash,
                         TxFailureDiagnostic diag) const
  {
    event_server_proxy_.report_tx_failure(std::move(tx_hash),
                                          std::move(diag));
  }
```

(`event_server_proxy_` is `PerTransactionEventServerProxy`'s existing `EventServerProxy &` member; calling a non-const method through it from a const member is fine.)

`hogd/hogexec/runtime/syscall_handler.h` — rename the declaration and rewrite its comment:

```cpp
  // On a failed 2pc push: trace-logs db_proxy_'s last_push_conflict() and
  // fire-and-forgets a best-effort TxFailureDiagnostic (first conflict +
  // executing block height) to the event server, which relays it to the
  // indexers so clients can learn why the tx was excluded (see
  // docs/superpowers/specs/2026-07-07-tx-failure-diagnostics-cache-design.md).
  // A no-op when hogdb captured no diagnostics (kDbDiagnosticNone, e.g.
  // validation/viewing blocks) -- hogdb selects the level from the block's
  // execution mode, so there is no seam to configure here. Called right
  // after each 2pc push failure site sets two_phase_commit_failed_. Never
  // blocks and never fails.
  void report_db_conflict_diagnostic() const;
```

`hogd/hogexec/runtime/syscall_handler.cc` — replace `log_db_conflict_diagnostic` (definition currently at line ~1050) with:

```cpp
void
SystemCallHandler::report_db_conflict_diagnostic() const
{
  auto const &conflict = db_proxy_.last_push_conflict();
  if (conflict.level() == DbDiagnosticLevel::kDbDiagnosticNone) {
    return;
  }
  trace("two-phase commit conflict diagnostics: {}", x23::show(conflict));

  // The wire format carries exactly one diagnostic (memory-bounded end to
  // end); under kDbDiagnosticAll (debug builds) send the first, drop the
  // rest.
  DbConflictDiagnostic const *first = nullptr;
  if (conflict.level() == DbDiagnosticLevel::kDbDiagnosticFirst) {
    first = &conflict.first();
  }
  else if (!conflict.conflicts().empty()) {
    first = &conflict.conflicts()[0];
  }
  if (first == nullptr) {
    return;
  }

  auto height = get_block_height();
  if (!height.has_value()) {
    // Tip read failed (block being torn down); drop the report.
    return;
  }
  event_server_proxy_.report_tx_failure(
      get_source_tx_hash(),
      TxFailureDiagnostic{.attempt_height = height.value(),
                          .conflict = *first});
}
```

Update the three call sites (two in `try_commit_transaction`, one in `commit_body_discarded`) from `log_db_conflict_diagnostic();` to `report_db_conflict_diagnostic();`.

- [ ] **Step 5: Build and run**

```bash
cmake --build ./build -j16 --target runtime_test
./build/hogd/hogexec/runtime/runtime_test --gtest_filter='TxFailureReportTest.*'
```

Expected: PASS for both instantiations (`MockInMemory` sends nothing; `HogDB` sends exactly one report). If the binary path differs, `find build -name 'runtime_test' -type f`.

- [ ] **Step 6: Run the full runtime suite to catch regressions**

```bash
./build/hogd/hogexec/runtime/runtime_test
```

Expected: PASS (the rename and the new fire-and-forget sends must not disturb any existing behavior — every proposal-mode 2PC-failure test in this binary now exercises `report_db_conflict_diagnostic`).

- [ ] **Step 7: Format and commit**

```bash
clang-format -i hogd/hogexec/runtime/event_server_proxy.h \
  hogd/hogexec/runtime/event_server_proxy.cc \
  hogd/hogexec/runtime/syscall_handler.h \
  hogd/hogexec/runtime/syscall_handler.cc \
  hogd/hogexec/runtime/test_util/test_event_aggregator.h \
  hogd/hogexec/runtime/test_util/test_event_aggregator.cc \
  hogd/hogexec/runtime/native_contract_test_framework.h \
  hogd/hogexec/runtime/tx_failure_report_test.cc
git add hogd/hogexec/runtime hogd/hogexec/runtime/CMakeLists.txt
git commit -m "hogexec: forward 2pc conflict diagnostics to the event server, best-effort"
```

---

### Task 6: Full-tree verification

**Files:** none new — verification only.

- [ ] **Step 1: Full build**

```bash
cmake --build ./build -j16
```

Expected: PASS with no warnings introduced by this work.

- [ ] **Step 2: Run the three affected test binaries in full**

```bash
./build/hogd/indexer/indexer_test
./build/hogd/event/event_test
./build/hogd/hogexec/runtime/runtime_test
```

Expected: all PASS. (Paths per Task 4/5 notes if they differ.)

- [ ] **Step 3: Run the broader suite via ctest to catch cross-component fallout**

```bash
ctest --test-dir build -j8 --output-on-failure
```

Expected: PASS. Investigate any failure before proceeding — especially anything touching `EventExecutor1`/`IndexerEvent1` dispatch or `TxInfoResult` consumers (`hogd/block_explorer/block_explorer.cc` and `hogd/devnode/hog_devnode_test.cc` use if/else on the outcome, not exhaustive switches, so they should be unaffected).

- [ ] **Step 4: Review the diff against the spec's invariants, then finish**

Re-read the "Error handling & trust" and "Out of scope" sections of the spec against `git diff main...HEAD`. Confirm: no send path can block proposal; no unbounded growth; committed record wins. Then the branch is ready for the user to push (protocol PR first, then hogd; groundhog-go bump after protocol merges — user-driven).
