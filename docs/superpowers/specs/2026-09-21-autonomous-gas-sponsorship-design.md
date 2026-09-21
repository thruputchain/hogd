# Autonomous gas sponsorship

**Date**: 2026-09-21
**Status**: Design — draft, not yet spec-reviewed
**Related**: `ExecResultFailedGasSponsorship = 100` (retired `GasClaim` /
`GasPermissionConfig`); `docs/superpowers/specs/2026-05-01-auth-claim-simplification-design.md`
("Gas sponsorship will be re-added later via wrapping envelopes")

## Problem

Groundhog has no fee market. Admission and scheduling priority come from
age-weighted HOG held at the invoked address: `HogAge` carries three groups
(`kHogAgeGroupToSchedule`, `kHogAgeGroupAlreadyScheduled`,
`kHogAgeGroupReceived`) that cycle Received → AlreadyScheduled → ToSchedule,
and `Scheduler::schedule()` spends `ToSchedule` hogs 1:1 against
`tx.tx.invocation.gas_limit` (`hogd/hog_mempool/scheduler.cc:1015-1020`).

This produces a cold start. A freshly funded address holds its hogs in
`kHogAgeGroupReceived`, which must cycle twice before they buy any scheduling
priority. A new user therefore cannot transact for some number of blocks after
being funded, no matter how much HOG they hold.

The previous answer to this — `GasClaim` in the transaction plus
`GasPermissionConfig` on `AddressAuthData` — required the submitter to obtain a
sponsor's **signature** before submitting. That is an out-of-band round trip to
a live service, with a liveness dependency, and it was removed in the
2026-05-01 auth-claim simplification.

The goal here is sponsorship with **no signature fetched from anyone**: the
submitter names who it is asking and for how much, and the named contract
decides autonomously, in its own code, during execution.

## Goals

- A transaction can be admitted and executed with zero scheduling priority of
  its own, funded by a contract that decides autonomously at execution time.
- No signature, and no network round trip, from any party other than the
  submitter.
- Bounded, budgeted cost for the unpaid work a proposer does on transactions
  that are never sponsored.
- No new consensus-visible tunable whose value proposers must coordinate on.

## Non-goals

- Fee markets, or billing gas in HOG generally. Gas calibration is still open
  (`hogd/hog_exec/runtime/syscall_handler.cc:321`); this design charges the
  same `ToSchedule` hogs the scheduler already spends.
- Signature-based sponsorship (a sponsor co-signing a specific transaction).
  That remains possible to add later as a separate wrapping envelope; it is a
  different mechanism with a different trust model.
- Multi-sponsor negotiation, partial grants, or a sponsor granting an amount
  other than the one the submitter declared.
- Sponsoring contract *deployment*. See "Lane restrictions".

## Design

### Transaction surface

`Transaction` gains a nullable `sponsorship` field. It also gains an
`ExtensionPoint`, which it lacks today unlike `ProposalHeader` /
`StorageCostModelXdr` / `ContractEvent`.

The extension point does not hold the sponsorship field. Per its contract in
`types.x`, an embedded `ExtensionPoint` costs four zero bytes now and lets the
*next* change replace the struct with a union carrying this one as variant 0.
Adding `sponsorship` is itself a flag day — it changes the signature preimage
and every `TransactionHash` — so the reserved field rides along on a break
that is already being taken, and buys the change after this one a union arm
instead of another flag day. Both are cheap only while the chain is
pre-mainnet.

```xdr
struct SponsorshipRequest {
  ExtensionPoint ext;

  /// SponsorGas fails unless the assenting frame's own address matches.
  ContractAddress sponsor;

  /// Must equal Invocation::gas_limit. Fixed at signing time, so the
  /// scheduler checks solvency against a known number.
  GasAmount amount;
};

/// Null = self-funded: full gas_limit from the start, scheduled against
/// the invoked address's HogAge, exactly as today.
typedef SponsorshipRequest *OptSponsorshipRequest;
```

Well-formedness, checked structurally before execution (so a violation is
`ExecResultMalformedTransaction` at proposal, `ExecResultTxValidationFailure`
at validation):

- `amount == invocation.gas_limit`. Anything less lets a sponsored
  transaction consume more gas than its sponsor committed to; anything more
  over-reserves the sponsor's hogs for gas the transaction cannot spend.

Because `SponsorshipRequest` sits inside `Transaction`, it is covered by the
signature preimage
(`cxs(DomainSeparator{u8"Transaction"}) || network_hash || xdr(tx)`). The
submitter has therefore **consented** to the named sponsor. Without that, any
contract in the call path could sponsor a transaction the user never offered
it, creating an unwanted on-chain association or debt relationship.

### The SponsorGas syscall

Because the amount is pre-declared, the syscall is an **assent**, not a grant.
The contract cannot name its own number.

```yaml
- name: "SponsorGas"
  # read_only omitted, per this file's convention for state-modifying
  # syscalls: it changes tx-level accounting, so it fails in a ReadOnly frame.
  needs_gas: true
  inputs:
    - name: "expected_amount"
      typename: "GasAmount"
  # errors: kSyscallErrorCodeSponsorshipNotOffered
  #         kSyscallErrorCodeSponsorshipAlreadyAssented
  #         kSyscallErrorCodeSponsorInsufficientHogs
```

Failures are syscall **error codes**, not frame faults, matching how
`kSyscallErrorCodeInvokeDepthExceeded` treats a resource limit as reportable
rather than malformed. Whether the not-offered case should instead be a hard
fault — a contract assenting to sponsorship it was not offered is arguably a
programming error rather than a condition to handle — is open.

Three properties follow:

1. **A contract can only ever sponsor itself.** Attribution is
   `GetCurrentAddress()`, which only that contract's own code controls. No
   contract can be made to sponsor a transaction by another contract in the
   call path.
2. **A contract cannot assent to an amount it did not check.** Passing
   `expected_amount` and erroring on mismatch means a blind assent is not
   expressible.
3. **A sponsor's exposure per transaction is fixed at signing time**, not at
   runtime, so the scheduler checks solvency against a known number rather
   than against a ceiling the runtime might later raise.

### Execution accounting

```
allowed_gas = <proposer-local initial allowance>
sponsor     = none

on SponsorGas(expected) from address A:
    require sponsor == none                           # else error
    require some term T in tx.sponsorship.terms has T.sponsor == A
    require expected == T.amount
    require A is solvent for T.amount                 # at grant time
    sponsor     = A
    allowed_gas = max(allowed_gas, T.amount)          # raises, never lowers

at every gas charge:
    if total_gas_consumed > allowed_gas: fault

on completion:
    sponsor != none  ->  debit sponsor for T.amount
    sponsor == none  ->  transaction is dropped, nobody pays
```

`allowed_gas` must only ever rise. If an assent could lower the ceiling, a
transaction that had legitimately spent gas under the initial allowance would
fault on gas it was entitled to spend.

Solvency is inherently point-in-time. `cycle_if_needed` moves hogs between
groups and other transactions drain `ToSchedule` between admission and
scheduling, so the admission check is a filter against obvious waste rather
than a guarantee. The binding checks are the existing
`gas_limit <= hogs_available` in `Scheduler::schedule()` and the grant-time
check below.

Solvency is checked **at grant time**, not only at commit. That is what lets
admission stay a cheap filter rather than a guarantee: a sponsor solvent at
admission but drained by the time it assents fails immediately, rather than
running to completion and discovering the shortfall at two-phase commit.
Commit-time insolvency remains possible — a sponsor solvent at grant can still
be drained by other transactions in the same block — and is already handled by
`kDbConflictBoundsExceeded` in `db_conflict.x` ("a nonnegative int or balance
would go below zero given the deltas in this block").

### The initial allowance is proposer-local

The initial allowance — the gas a transaction may burn *before* reaching a
`SponsorGas` call — is **not consensus-visible**, and needs no coordination.

It only ever binds on transactions that are dropped, and dropped transactions
are not in the block. Any transaction that *is* included demonstrably reached
`SponsorGas` before its allowance ran out, and from that point `allowed_gas` is
the declared amount. So the initial allowance is never the binding
constraint on anything a validator replays.

A stingier proposer includes fewer sponsored transactions; a more generous one
includes more. Both produce valid blocks. Proposers may tune it freely and may
compete on generosity.

### Why validators need a bound anyway

Validators re-execute every transaction in a block. If a sponsored transaction
could sit in a block with no bound on its pre-grant phase, a malicious proposer
could include one carrying a large `invocation.gas_limit` that never calls
`SponsorGas`: the proposer pays once, and the entire validator set burns that
gas with nobody to charge.

Pre-declaring the amount closes this without reintroducing a consensus-visible
allowance. **Validators replay a sponsored transaction bounded by `amount`**,
which equals `invocation.gas_limit` — the same bound that already applies to
every self-funded transaction, on a value the submitter signed. No
proposer-supplied hint in `NondeterministicProposalData` is required.

No new parameter caps it, and none is needed. `gas_limit` has no protocol
bound today and does not want one: it is gated economically, because
`Scheduler::schedule()` will not schedule a transaction unless the paying
account holds `gas_limit` hogs in `kHogAgeGroupToSchedule`. A sponsored
transaction is that same bound with a different payer, so the sponsor's
solvency check is what bounds validator replay. A rich sponsor naming a large
amount is paying for it, exactly as a rich account self-funding a large
transaction does.

### Included-but-ungranted transactions

With the replay bound in place, a sponsored transaction that reaches a block
without an assent costs validators at most the declared amount. Rejecting it is
therefore a policy choice rather than a safety requirement.

**Decision: reject the block.** A transaction that charges nobody has no
legitimate reason to be included, and admitting them lets proposers free-ride
on validator CPU even within the bound. It also prevents a proposer burning a
submitter's `Uniquifier` for free.

This needs exec results in the *exclusion* class, not the includable one.
`ExecResultTxDeterministicError = 1` is explicitly includable, so running out
of initial allowance before an assent must not use it:

- **Proposal**: exclusion-class result, the `ExecResultMalformedTransaction = 201`
  shape — "the scheduler drops the transaction instead of including it in the
  block".
- **Validation**: `ExecResultTxValidationFailure = 200` — a sponsored but
  ungranted transaction inside a proposed block is the proposer's fault.

This mirrors the split already documented for malformed transactions.

### Lane restrictions

A sponsored transaction is restricted until its assent lands. All of these lift
the moment `SponsorGas` succeeds, so they are cheap to enforce as pre-grant
state:

- `contracts_to_deploy` must be empty. Deployment costs
  `base_contract_deployment_cost + contract_deployment_cost_per_byte * size`
  and is a straightforward amplification vector.
- No retrocausal-queue reads. `kMaxConcurrentRcqReadsPerTx` is 16384; that is
  far too much unfunded buffering to hand out.
- No `TryInvoke`. It acquires two-phase-commit locks and appends to
  `nondeterministic_bits`.

### Mempool admission

```
tx.sponsorship == null
  -> today's path: bucket under invoked_address, admit iff its
     ToSchedule hogs cover invocation.gas_limit

tx.sponsorship != null
  -> read the named sponsor's HogAge alongside the invoked address's
  -> admit to the sponsored lane iff the sponsor's ToSchedule hogs cover
     `amount`
  -> schedule against the sponsor's HogAge, debiting `amount`
```

The sponsored lane is a **parallel structure**, not a modification of the
existing one: a second priority set keyed by sponsor address, drawn from under
a quota. Keying the lane by sponsor means `schedule()` holds exactly one
account lock per entry, mirroring the existing loop and avoiding cross-account
lock ordering inside the per-account critical section.

Two quotas bound it. Both are **node-local policy, not `ConfigParam`s**: like
the initial allowance, they only ever bind on transactions a proposer drops,
which never reach a block, so no validator checks them and nothing needs
coordinating.

- Sponsored-lane block-slot share, so sponsored traffic cannot crowd out
  self-funded traffic.
- Sponsored-lane per-block gas budget: the ceiling on proposer execution spent
  on sponsored-lane attempts. This is the real bound on unpaid work, and it is
  a gas budget rather than a transaction count because gas is what the attack
  actually consumes.

This design adds **no `ConfigParam` at all**. The bound validators need comes
from `amount == gas_limit` plus the sponsor's solvency, both of which already
exist.

Per-address rate limiting is deliberately **not** part of this. Addresses
reached through the `kPubkeySignature` claim are
`SHA256(cxs(DomainSeparator{u8"PubkeyAccount"}) || cxs(preimage))`, so an
attacker grinds fresh ones for free. The global gas budget is the defense.

Stated plainly: the chain spends a fixed slice of every block's execution on
free onboarding, and an attacker can waste that slice. The self-funded lane is
untouched. That is a product decision about what onboarding is worth, not an
open-ended vulnerability — and it is available precisely because the proposer
has a protocol-level budget, which an ERC-4337 bundler does not.

## Sizing the initial allowance

The allowance must carry execution from the root invocation to a `SponsorGas`
call:

```
root invocation -> load the user's account contract -> its code runs
                -> Invoke the sponsor -> load the sponsor
                -> sponsor reads its policy -> SponsorGas
```

At `kSyscallBaseCost = 50` the syscall crossings are noise; contract loading
and two RV32 bodies dominate, with `kDefaultMaxRv32CodeBytes = 65536` bounding
the worst case.

**Bootstrap with `ContractType::kNative` sponsors.** A precompiled sponsor
skips RV32 load and guest execution entirely, so the allowance only has to
cover reaching it. Ship the mechanism there, measure real gas-to-`SponsorGas`
on an RV32 path, and size from measurement.

Keep the allowance in **network config, not protocol constants** — the gas
schedule is digest-pinned to `rv32::kGasScheduleDigest`, and retuning the
allowance must not require a coordinated gas-schedule update.

## Affected code

### Protocol (`protocol/`)

- `transaction.x` — `ExtensionPoint` on `Transaction`; `SponsorshipRequest`,
  `OptSponsorshipRequest`.
- `syscalls.yaml` — the `SponsorGas` entry.
- `syscall_error.x` — `kSyscallErrorCodeSponsorshipNotOffered`,
  `kSyscallErrorCodeSponsorshipAlreadyAssented`,
  `kSyscallErrorCodeSponsorInsufficientHogs`.
- `exec_result.x` — nothing. The validator side is already
  `ExecResultTxValidationFailure`, and the proposal side is a scheduler
  decision about what to include rather than an enum value.
- `network_config.x` — nothing. The lane quotas and the initial allowance are
  node-local and belong in hogd's configuration.
- `sched_gateway.x` — a `SchedGatewaySubmitOutcome` variant for "no declared
  sponsor is solvent".

### Mempool (`hogd/hog_mempool/`)

See the companion scope notes. The load-bearing change is that the
**scheduling account and the identity account diverge** for sponsored
transactions: `add_txs_helper`, `is_pending`, `reenqueue_txs`, and
`get_txs_for_account` all key on `tx.tx.invocation.invoked_address` today.

### Executor (`hogd/hog_exec/`)

- `SponsorGas` syscall handler; `allowed_gas` tracking alongside the existing
  `gas_consumed_` accounting in `syscall_handler.cc`.
- Pre-grant restriction enforcement (deploys, RCQ, `TryInvoke`).
- Validator-side replay bound of `amount`.

## Testing

- A brand-new address with zero hogs transacts successfully via a native
  sponsor; the sponsor's `ToSchedule` hogs decrease by exactly `amount`.
- `SponsorGas` from an address not named in `terms` faults.
- `SponsorGas` with `expected_amount != sponsorship.amount` errors.
- A second `SponsorGas` in one transaction faults.
- `SponsorGas` inside a `ReadOnly` frame faults.
- A transaction that never assents is dropped, not included, and its
  `Uniquifier` is not consumed.
- A block containing a sponsored-but-ungranted transaction is rejected by
  validators.
- Two proposers with different initial allowances both produce valid blocks;
  a transaction included by the generous one validates against a node whose
  local allowance is smaller.
- A sponsor drained by earlier transactions in the same block causes later
  ones to fail with `kDbConflictBoundsExceeded`.
- `amount != invocation.gas_limit` is rejected as malformed.
- Sponsored transactions cannot exceed `kConfigParamSponsoredLaneFraction` of
  a block.
- Pre-grant `contracts_to_deploy`, RCQ reads, and `TryInvoke` all fault.

## Wire-compatibility cost

`scripts/xdrcompat` reports **66 violations** against `protocol` main for the
sketch (26 R1 procedure-signature changes, 40 R3 layout changes). The check is
advisory in CI. Two independent causes:

- **`Transaction` layout** (27 reports). Every RPC carrying a
  `SignedTransaction` or `ProposedTransaction` — auth, mempool, scheduler,
  indexer, event, loadgen, wallet, repair — changes signature. This is the
  flag day, and it is the cost of the change whether or not the
  `ExtensionPoint` rides along.
- **`ConfigParam` gaining an arm** (20 reports). Every RPC carrying a
  `NetworkConfig`, `ConfigHashPreimage` or `DbTip` changes signature. This is
  not specific to sponsorship: `network_config.x` documents the union as
  designed to grow ("a new parameter is then an id and a `ConfigParam` arm"),
  so **any** new parameter costs this. Worth confirming with the team that
  these reports are understood as routine rather than blocking, since the
  checker cannot tell the intended growth from an accident.
