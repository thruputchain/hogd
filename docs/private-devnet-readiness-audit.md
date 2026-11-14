# Executor and database private-devnet readiness review

Review date: 2026-09-11. Code baseline: `a6887a7420013cf46a92478fc12b6549f9a274c5`.
This is a source review of `hog_exec` and `hog_db`, with selected runtime,
protocol, configuration, recovery, and RPC dependencies. It is not a complete
security audit or evidence that a deployed network passed the gates below.
Open PRs were checked to avoid duplicating work; their status can change.

## New focused fixes

| Priority | Finding and concrete consequence | Draft |
|---|---|---|
| Before persisted devnet use | `hog_db_init` treats every failed `read_dbtip()` as fresh state, and `StartupValidation::tip_is_servable` accepts negative committed heights. Decode/type failures must not become genesis; persisted negative heights must not reopen bootstrap behavior. | [#3976](https://github.com/groundhogchain/hogd/pull/3976) |
| Before exposing views | `ViewGatewayRpc` forwards caller gas into `View::run_view_transaction`, outside scheduler balance admission. `UINT64_MAX` allows effectively unlimited metered execution. Add a configurable local ceiling. | [#3977](https://github.com/groundhogchain/hogd/pull/3977) |
| Before exposing storage reads | `HogDbGatewayRpc::dbgateway_get_items` reserves and materializes the whole batch, including repeated copies of a large value. Bound count before reads and serialized result bytes during assembly. | [#3978](https://github.com/groundhogchain/hogd/pull/3978) |

The new defaults are starting policies, not measured capacity guarantees.
Review the view ceiling against actual contract workloads and align public
gateway aggregation with the database item/byte limits before rollout.
The current wire protocol lacks a dedicated resource-limit result for batch
reads, so the database draft uses its existing outer server-error result.
Operator/client handling is part of adopting that change.

## Configuration inventory

Consensus-affecting limits belong in the chain configuration or a coordinated
protocol version. Independently tunable node flags must not decide whether a
proposed transaction succeeds. Local read-service limits can differ by node
because public views and gateway reads do not commit state.

| Parameter family | Baseline and action |
|---|---|
| Contract bytes, savepoint depth, SHA buffer bytes, auth-token depth, RV32 code/memory | Existing [#3943](https://github.com/groundhogchain/hogd/pull/3943) migrates six limits to tip-pinned configuration and updates protocol/bindings. Review its dependencies and genesis validation instead of creating a competing migration. |
| SHA buffer count, cross-invoke depth, new-contract count, bytestring length | Already resolved by `NetworkConfigParams`; record explicit genesis values and the committed config hash. Do not independently retune fallback defaults for an existing chain. |
| Host syscall gas and initial contract verification | `gas_cost_table.h` remains compiled policy; `SystemCallHandler::root_invoke` explicitly records zero verification gas and unfinished calibration. Finish calibration and define a coordinated schedule/version strategy before treating gas as a reliable host-work budget. |
| Transaction aggregate memory and RCQ work | `kExecToDbMessageCap` is a 16 MiB execution admission budget, while bulk transport can frame much more. It affects transaction outcomes and is not an ordinary node-local RPC tuning knob. Review the retained-state and RCQ caps below together. |
| View gas | Local flag in #3977. Up to three attempts can each spend the ceiling. Unmetered host work and concurrent requests remain separate budgets. |
| Database gateway batch count/bytes | Local flags in #3978. Byte cap includes XDR overhead. Gateway batching must respect them; splitting changes the atomic snapshot boundary. |
| Snapshot-cache retention / refresh | `ExecutorStateManager` retains up to 16 snapshot caches; view refresh probes every 100 ms and retries at most three times. These are reasonable future operational settings, but measure memory per retained snapshot and freshness/retry rates before widening them. Entry count alone is not a byte bound. |
| Worker capacity | `hog-exec --nthreads` is explicitly ignored; execution uses the shared x23 pool. Existing deployment sizing that relies on this flag is ineffective. Verify the actual pool and container CPU settings under mixed transaction/view load. |
| Listener placement | Both daemons use one `--bind-ip` for metrics and service listeners. Use explicit addresses and network policy; consider separate metrics/view bind settings if those surfaces need different exposure. |

## Existing fixes to review as a release set

These links identify relevant work already open at review time. They are not
claims that the PRs are correct, current, or sufficient without integration
testing. Resolve alternative/superseding drafts rather than merging both.

| Area | Existing work | Why it matters |
|---|---|---|
| Deterministic transaction size gate | [#3973](https://github.com/groundhogchain/hogd/pull/3973), superseding executor-relevant rationale in [#3845](https://github.com/groundhogchain/hogd/pull/3845) | Size decisions must use the committed transaction identity, not whichever signature wrapper a node retained. |
| Retained bytestring writes | [#3682](https://github.com/groundhogchain/hogd/pull/3682) | Admission only at final DB push is too late to bound memory retained while executing. Reconcile the patch with current syscall gas changes. |
| RCQ cached reads | [#3532](https://github.com/groundhogchain/hogd/pull/3532) | Past-block reads retain full decoded queue blocks across transactions. A per-transaction count still needs a whole-block/concurrent-view capacity test. |
| RCQ deletion fan-out | [#3671](https://github.com/groundhogchain/hogd/pull/3671), superseding [#3626](https://github.com/groundhogchain/hogd/pull/3626) | A small delete can expand into many row removals at block close. Its description explicitly leaves delete/recreate and consume-to-tail enumeration as follow-up work. |
| Auth-token retention | [#3827](https://github.com/groundhogchain/hogd/pull/3827) | Finite mint count is useful, but inspect total decoded tree bytes, unwrap growth, and forwarded view tokens too. Count and depth alone are not total memory budgets. |
| Child-frame host work | [#3841](https://github.com/groundhogchain/hogd/pull/3841), [#3409](https://github.com/groundhogchain/hogd/pull/3409) | Ancestor merges and RV32 image setup need deterministic charging. Confirm compatibility with runtime performance changes. |
| Recovered metadata | [#3953](https://github.com/groundhogchain/hogd/pull/3953), [#3696](https://github.com/groundhogchain/hogd/pull/3696), [#3690](https://github.com/groundhogchain/hogd/pull/3690) | Future ages and replay/storage-cost arithmetic need semantic validation in addition to decoding. |
| RPC framing and buffering | [#3214](https://github.com/groundhogchain/hogd/pull/3214), [#3570](https://github.com/groundhogchain/hogd/pull/3570), [issue #3086](https://github.com/groundhogchain/hogd/issues/3086) | Rejecting oversized records does not bound total queued replies to a slow reader. The new handler caps do not solve that transport problem. |
| Startup and shutdown | [#3489](https://github.com/groundhogchain/hogd/pull/3489), [#3763](https://github.com/groundhogchain/hogd/pull/3763) | Executor registration and draining in-flight responses affect safe restart behavior. Follow the component failure model rather than relying on graceful shutdown. |

The baseline already checks recovered configuration hashes/default-wallet
presence and guards maximum-height snapshot creation. Those are not new
findings from this review, even where older open issues/PRs describe them.
Local hashes detect inconsistency, not provenance: a self-consistent malicious
replacement of both state and local metadata still needs an independently
trusted recovery checkpoint/procedure.

## Required private-devnet acceptance evidence

Leave every item unchecked until it has been performed on the chosen release
and deployment. The code review itself satisfies none of these deployment gates.

- [ ] Pin the hogd commit, all submodule commits, CLI/lowerer/SDK versions, and
  genesis/config/network hashes for every validator. Run the same signed
  transactions through proposal and validation on multiple nodes, including
  alternate valid signature wrappers and boundary-size contracts.
- [ ] Restrict DB executor/scheduler/auth ports, executor coordinator RPC, and
  metrics to intended peers. Protocol-intro equality checks are compatibility
  checks, not peer authentication. Test reachability from an untrusted client
  network; expose only the intended authenticated/rate-limited gateway.
- [ ] Run C++ Debug/Release and relevant sanitizer checks for the integrated
  release. Include `hog_db_test`, `exec_interface_test`, runtime and RCQ tests,
  and multi-node payment/contract execution with matching committed roots.
- [ ] Run the manual `Node Startup Order (Namespace)` workflow, including
  starved dependencies and indexer/event orderings. Its workflow is manually
  dispatched, so ordinary PR CI is not evidence these scenarios passed.
- [ ] Exercise unclean termination during DB/event close and commit, restart
  on intact volumes, and verify coordinator, DB, event, and indexer heights and
  hashes reconcile before readiness. Use the existing tests and expectations in
  [component-failure-model.md](component-failure-model.md).
- [ ] Rehearse a restore from an accepted checkpoint on replacement volumes.
  Preserve damaged data for diagnosis; do not treat deleting state and silently
  rerunning genesis as recovery. Record the supported clean-volume migration
  policy before shipping an on-disk format.
- [ ] Run bounded adversarial load: high-gas views, large duplicate read
  batches, slow readers, repeated view retries, many storage keys, broad auth
  tokens, and long-lived RCQs. Record RSS, latency, worker saturation, commit
  duration, WAL/disk growth, and recovery time. Configure gateway concurrency
  and output backpressure from those results.
- [ ] Verify readiness falls when required peers fail and returns only after
  reconciliation. Startup `kOk` alone is not a continuous dependency-health
  signal. Alert on stalled block progress, DB/event mismatch, indexer lag,
  low disk/memory, and repeated unrecoverable execution/RPC errors.
- [ ] Test developer-facing failures: bad artifact, out-of-gas, over-limit
  view/read request, and temporarily unavailable dependencies. Current errors
  collapse multiple causes into `Fault`/server error; document actionable client
  behavior until the wire API gains richer outcomes.

## Validation limits of this review

The new code drafts include focused regression tests and were checked with
clang-format 21 and `git diff --check`. This environment has no CMake or
initialized private build dependencies, so no local C++ test pass is claimed.
Repository CI was triggered by the draft PRs; consult their latest checks for
results. No validators were deployed, no network load test was run, and no
existing security draft was merged as part of this scan.
