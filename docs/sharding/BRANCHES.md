# hogdb sharding — branch map and what each one still needs

Written 2026-08-21 as Vinita's internship wrapped up. Everything about hogdb
sharding that is not in a single reviewable PR lives on one of the branches
below. Read this first; it says which branch to pick up and what state it is in.

Benchmark results and the campaign narrative are in
[HANDOFF.md](HANDOFF.md). Known defects are in [ISSUES.md](ISSUES.md).

---

## The one thing that gates everything

**The sharding PR cannot merge until the protocol change merges.** hogdb sharding
needs shard identity on `ShardCard`, which is
[protocol PR #133](https://github.com/groundhogchain/protocol/pull/133)
(`vp/shard-card-identity`) — still open. Until it lands, every hogd branch here
pins `protocol` (and, through `protocol-go`, `groundhog-go`) at a *branch*
commit rather than at protocol `main`.

The merge order is therefore fixed:

1. Merge protocol PR #133.
2. Re-pin hogd's `protocol` submodule to protocol `main`.
3. Regenerate `protocol-go` bindings against that and re-pin `groundhog-go`
   (see "Regenerating groundhog-go" below — there is a locale trap).
4. Land the storage-filler contract, or drop it from the branch.
5. Then merge the hogd sharding PR.

---

## hogd branches (`github.com/groundhogchain/hogd`)

### `vp/sharding-v2-db-abstraction` — the PR branch

**[Draft PR #3108 "Hogdb sharding"](https://github.com/groundhogchain/hogd/pull/3108).**
47 commits on top of `main`, rebased onto `main` on 2026-08-21.

This is the branch to review and merge. It contains the whole sharding
implementation: shard routing, the `ShardedDbSched` facade, sharded genesis
(both the v1 and v2 ceremonies), the sharded auth server, the sharded hogdb and
hogexec gateways, shard identity persistence, and `hog-devnode --num-db-shards`.

Submodule pins, all of which are *branch* commits and all of which must move
before merge:

| submodule | pinned commit | branch |
|---|---|---|
| `protocol` | `81e4b2f` | `vp/shard-card-identity-on-main` |
| `groundhog-go` | `8cbd6bd` | `vp/shard-card-identity-on-main` |
| `built_contracts/integration_test_contracts` | `58052bb` | `vp/storage-filler-on-main` |

Each of those three is the shard-card / storage-filler work rebased onto exactly
what hogd `main` pins, so the branch builds against current `main` rather than
dragging an old protocol along with it.

### `vp/devnode-db-shards` — mostly in the PR, but not entirely

The `hog-devnode --num-db-shards N` work: N in-process db shards, the
`dbGateway`/`viewGateway` fan-out, the shard-identity resolution shared with
hogdb, and the mempool genesis race fix.

Most of it is already in the PR branch, swept in by the
`sharding: checkpoint the tree the benchmark campaign is running` commit — but
**the viewing-snapshot work is not.** `shard_block_id` and
`ShardedViewGatewayViewsWalletsOnEveryShard` exist nowhere on the PR branch
(checked 2026-08-21), so a sharded devnode there can serve accounts through the db
gateway but cannot *view* state a non-primary shard owns. Porting those last two
commits across is real outstanding work, and it is the only reason this branch is
still alive. See
[DEVNODE-SHARDS.md](https://github.com/groundhogchain/hogd/blob/vp/devnode-db-shards/docs/sharding/DEVNODE-SHARDS.md)
on that branch for the diagnosis and what not to regress.

Pick this branch up if you need to test sharding through devnode rather than
through a nine-host cluster run — devnode is the only way to get a sharded
runtime manifest into hogsim.

### `vp/obj` — ObjecTier on ramdb, an exploration

31 commits, ~8.9k lines, branched off `main` (and now ~123 commits behind it).
Self-contained: every submodule is on upstream `main`, so it clones and builds
without any of the pinning above.

Not a merge candidate as it stands — it is a measurement harness plus a working
mechanism, deliberately without a policy layer. See
[docs/objectier/STATUS.md](../objectier/STATUS.md) on that branch for where it
got to, what it proved, and what the next step is.

### `vp/sharding-v2-archive` — this branch

The reports, the cleaned issue log, and the benchmark-harness changes to tracked
files that the campaign ran with but that should not gate PR review. Never a
merge candidate; it exists so none of it is lost.

---

## Submodule branches

All pushed, so any branch above can be cloned and initialised. Three families,
and the `-on-main` member of each is what the PR uses:

**`protocol`**

| branch | commit | what it is |
|---|---|---|
| `vp/shard-card-identity` | `546f079` | the open PR #133 |
| `vp/shard-card-identity-rebase` | `544333a` | the same work, rebased for the 2026-08-18 hogd rebase |
| `vp/shard-card-identity-on-main` | `81e4b2f` | **what the PR pins** — rebased onto `3d80720`, the protocol commit hogd `main` pins |

**`groundhog-go`** (and `protocol-go` inside it)

| branch | commit | what it is |
|---|---|---|
| `vp/shard-card-identity-benchmarked` | `20075e3` | the pin the benchmark campaign actually ran |
| `vp/shard-card-identity-rebase` | `6d422de` | the 2026-08-18 rebase's pin |
| `vp/shard-card-identity-on-main` | `8cbd6bd` | **what the PR pins** — one commit on hogd `main`'s `b46aab4`, moving only `protocol-go` |
| `protocol-go` `vp/shard-card-identity-on-main` | `30f6b3f` | bindings regenerated against protocol `81e4b2f` |

**`built_contracts/integration_test_contracts`**

| branch | commit | what it is |
|---|---|---|
| `vp/storage-filler` | `66e228b` | the `storage_filler` contract |
| `vp/storage-filler-benchmarked` | `ac45e22` | the pin the campaign ran |
| `vp/storage-filler-on-main` | `58052bb` | **what the PR pins** — rebased onto `76fecfd` |

`storage_filler` is a contract that writes state on demand, built for the
capacity experiments. It was never merged anywhere. If it is not wanted upstream,
dropping it from the PR removes this whole submodule pin.

---

## Regenerating groundhog-go (the locale trap)

`protocol-go/build.sh` builds its `goxdr` command line from a `protocol/*.x`
glob, and **shell glob order depends on the locale**. Run it under the default
`C.UTF-8` and every declaration in `generated.go` is re-sorted: a 12,000-line
diff that hides the ~80 lines you actually changed.

```sh
cd groundhog-go/protocol-go
LC_ALL=en_US.utf8 PATH="$PATH:$(go env GOPATH)/bin" ./build.sh
```

`goxdr` must be on `PATH` (`go env GOPATH`/bin). The `RFC4506 disallows comma
after last enum tag` lines are pre-existing warnings, not errors.

---

## Building any of these branches

- **`gcc-15`, not the default `g++`.** `scripts/conf.sh` pins `gcc-15`/`g++-15`
  because the machine default is now GCC 16, which fires false-positive
  `-Werror` diagnostics on the pinned rocksdb headers. Use
  `./scripts/build.sh -c gcc`. `scripts/conf.sh` is gitignored, so a fresh
  worktree needs it written by hand.
- **Run `./scripts/fetch.sh` after switching branches.** The rv32 contract build
  checks the `groundhog` cargo toolchain version and fails the whole build with
  `expected 'groundhog 0.9.0', got 'groundhog 0.5.12'` if it is stale. The
  post-checkout hook normally does this for you.
- **The post-checkout hook re-inits submodules on every checkout**, which detaches
  them from the branches in the table above. Re-check `git submodule status`
  after any checkout; the pinned commits are right even when the branch names are
  gone.
