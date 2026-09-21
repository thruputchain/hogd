# Autonomous gas sponsorship: mempool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the scheduler admit and schedule a transaction against a *named
sponsor's* hogs rather than the invoked address's, per
`docs/superpowers/specs/2026-09-21-autonomous-gas-sponsorship-design.md`.

**Architecture:** Today the scheduling account, the auth account and the
identity account are all `tx.tx.invocation.invoked_address`. Sponsorship splits
them: the sponsor pays for scheduling, the invoked address keeps auth and
replay identity. Sponsored transactions are therefore **dual-indexed** — they
stay in the invoked address's bucket (so the auth-invalidation sweep, dedup and
reenqueue paths are untouched) and additionally enter a sponsor-keyed lane that
`schedule()` drains under a quota.

**Tech Stack:** C++23, the `x23` XDR framework (`.x` schemas compiled to `.hh`
by `x23c` at build time), GoogleTest.

## Global Constraints

- **Blocked on protocol.** `SponsorshipRequest` does not exist until the
  protocol sketch lands and the submodule is bumped. Until then this plan does
  not compile.
- **`Transaction` gains an `ExtensionPoint` and a `sponsorship` field.** That
  is a flag day: 93 files construct `Transaction{...}` and every one needs the
  new leading field. Land that mechanical change **by itself**, before any task
  here, so its diff stays legible.
- **One account lock per iteration.** `Scheduler::schedule()` currently holds
  exactly one account lock at a time, and the comments at `add_txs_helper`
  (`scheduler.cc:438`) and `reenqueue_txs` document account-then-cache
  ordering. Keying the sponsored lane by sponsor and draining it in its own
  loop preserves that. Do not acquire a sponsor's lock while holding the
  invoked address's.
- **Lane quotas are node-local, not `ConfigParam`s.** They only bind on
  transactions the proposer drops, which never reach a block.

---

## The coupling that makes this non-trivial

`get_txs_for_account` (`scheduler.h:117`) is not a lookup convenience.
`rpc_from_coord.cc:3232-3245` calls it whenever an address's `AddressAuthData`
changes on chain: it pulls every pending transaction for that address, re-runs
`authmempool_verify_transactions`, and removes the ones that no longer
authenticate.

That sweep keys on the **auth** address. If sponsored transactions were moved
into the sponsor's bucket, a transaction whose signing key was just revoked
would become invisible to it and stay in the mempool. That is a security bug,
not a missing feature, and it is why the primary bucket must stay on
`invoked_address`.

---

## File Structure

- `hogd/hog_mempool/scheduler.h` — `SponsorLaneKey`; `ready_sponsors_`;
  declarations for the sponsor-lane helpers.
- `hogd/hog_mempool/scheduler.cc` — sponsor collection in
  `read_average_hog_ages`; dual insert in `add_txs_helper`; sponsor debit in
  `schedule()` and `apply_committed_block_hog_ages`; sponsor routing in
  `reenqueue_txs`; sponsor entries in the pending-nomination maps.
- `hogd/hog_mempool/rpc_from_coord.cc` — sponsored-lane capacity sub-limit;
  plumb `SchedGatewaySubmitSponsorshipUnfunded`.
- `hogd/hog_mempool/shared.h` — node-local lane configuration.
- `hogd/hog_mempool/test/` — new scheduler tests.

**Explicitly unchanged:** `hogd/hog_auth/*` (sponsorship adds no signatures and
does not touch `AuthClaim`, so `authmempool_verify_transactions` is unaffected),
`hash_transactions` / `TransactionHash` (the request lives inside `Transaction`
and is covered automatically), and the shape of the `submit_unvetted` DAG.

---

## Task 1: Sponsor-aware hog-age reads (no behavior change)

Teach `read_average_hog_ages` to collect sponsor addresses alongside invoked
addresses, so the batch read covers both. No scheduling change yet; sponsored
transactions cannot exist until the protocol lands, so this is inert.

**Files:**
- Modify: `hogd/hog_mempool/scheduler.cc` (`read_average_hog_ages`, all three
  overloads — `ContractAddressBatch`, `SignedTransaction` batch,
  `HogsTransferred`)
- Test: `hogd/hog_mempool/test/`

**Steps:**
- [ ] At `scheduler.cc:299`, collect `terms[*].sponsor` in addition to
      `invocation.invoked_address`.
- [ ] Confirm `dbsched_get_hog_ages(ContractAddressBatch)` needs no interface
      change — it already takes a batch, so the extra addresses ride along.
- [ ] Distinguish "sponsor unknown" from "invoked address unknown" in the
      `missing` set, since they produce different submit outcomes.
- [ ] Test: a batch mixing sponsored and self-funded transactions issues one
      DB read covering every distinct address exactly once.

---

## Task 2: Dual indexing (structural, lane still empty)

Add the sponsor-keyed lane and keep it consistent with the primary bucket, with
nothing entering it yet. Purely structural and testable on its own.

**Files:**
- Modify: `hogd/hog_mempool/scheduler.h`, `hogd/hog_mempool/scheduler.cc`

**Steps:**
- [ ] Add `ready_sponsors_`, ordered like `ready_accounts_` — the `AccountKey`
      comparator sorts by `average_hog_age` then address, and the sponsor lane
      wants the same ordering over sponsor addresses.
- [ ] `add_txs_helper`: keep the `invoked_address` insert; additionally insert
      into the sponsor lane when `tx.tx.sponsorship` is non-null.
- [ ] `remove_from_scheduler` and the `schedule()` erase paths: clear **both**
      indexes. A lane entry outliving its primary entry is the failure mode to
      test for.
- [ ] `reenqueue_txs` (`scheduler.cc:1167`): route sponsored transactions back
      into both.
- [ ] Verify `is_pending` (`scheduler.cc:458`) still answers correctly from the
      primary bucket alone — `rpc_from_coord.cc:438` depends on it for capacity
      dedup.
- [ ] Test: insert/remove/reenqueue of a sponsored transaction leaves the two
      indexes consistent; `get_txs_for_account(invoked_address)` still finds it.

---

## Task 3: Admission

Filter on sponsor solvency and report the new outcome.

**Files:**
- Modify: `hogd/hog_mempool/scheduler.cc` (`add_txs`),
  `hogd/hog_mempool/rpc_from_coord.cc` (capacity node)

**Steps:**
- [ ] In `add_txs`, admit a sponsored transaction iff at least one named
      sponsor's `kHogAgeGroupToSchedule` covers that sponsor's own term amount.
- [ ] Reject with `SchedGatewaySubmitSponsorshipUnfunded` otherwise.
- [ ] Add a sponsored-lane capacity sub-limit in the `enforce capacity` node,
      separate from `mempool_tx_size_limit_`, so sponsored traffic cannot fill
      the mempool and starve self-funded traffic at admission. This is a
      different layer from the block-slot quota in Task 4.
- [ ] Note in comments that this check is point-in-time: `cycle_if_needed` and
      concurrent draws can invalidate it before scheduling. It is a filter, not
      a guarantee.
- [ ] Test: unfunded sponsor rejected; one-solvent-of-two admitted; sponsored
      transactions cannot exceed the sub-limit.

---

## Task 4: Scheduling

Drain the sponsor lane under quota, debiting the sponsor.

**Files:**
- Modify: `hogd/hog_mempool/scheduler.cc` (`schedule`,
  `apply_committed_block_hog_ages`, pending-nomination helpers)

**Steps:**
- [ ] Add a second loop in `schedule()` over `ready_sponsors_`, holding only
      the sponsor's lock, resolving transaction bodies through `txs_` by hash.
- [ ] Debit `term.amount` from the sponsor's `kHogAgeGroupToSchedule`, mirroring
      `scheduler.cc:1015-1020` but charging the term rather than
      `invocation.gas_limit`.
- [ ] Apply the block-slot share and the per-block sponsored-lane gas budget.
- [ ] `apply_committed_block_hog_ages` (`scheduler.cc:218`): debit the sponsor,
      not the invoked address, for committed sponsored transactions.
- [ ] Pending nomination: `get_hog_age_from_pending_nomination_create_if_needed`
      and `accounts_excluded_from_block` must carry sponsor entries through
      `start` / `commit` / `abandon`.
- [ ] Test: sponsor's `ToSchedule` decreases by exactly `term.amount`; the
      invoked address's hogs are untouched; abandoning a nomination restores
      the sponsor's hogs; the lane cannot exceed its block-slot share.

---

## Task 5: Metrics

**Steps:**
- [ ] `MempoolMetrics`: sponsored-lane pending depth, admissions rejected for
      unfunded sponsors, lane gas budget consumed per block.

---

## Open questions for review

- **Lane ordering.** `ready_sponsors_` ordered by the sponsor's average hog age
  means a well-funded sponsor's traffic always wins. Round-robin across
  sponsors may be fairer. Unresolved.
- **Multiple terms.** The design permits up to `kMaxSponsorTerms` sponsors, but
  only one assents. Whether the lane should be keyed on all named sponsors or
  only the first solvent one changes both the index shape and the fairness
  story.
- **`kPubkeySignature` addresses are Sybil-cheap** — they are
  `SHA256(cxs(DomainSeparator{u8"PubkeyAccount"}) || cxs(preimage))` — so
  per-address rate limiting in the lane buys nothing. The design leans entirely
  on the global gas budget. Confirm that is acceptable.
