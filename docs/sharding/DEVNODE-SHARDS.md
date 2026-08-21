# hog-devnode with N db shards — state of this branch

Branch `vp/devnode-db-shards`. Written 2026-08-21 as Vinita's internship wrapped
up.

This is the branch that makes `hog-devnode` run its state db as N shards in one
process, so sharding can be exercised without booking nine cluster hosts. It is
also **the only way to feed a sharded runtime manifest to hogsim.**

## Read this first: most of it is in the PR, one part is not

Most of this branch was swept into the sharding PR
([hogd #3108](https://github.com/groundhogchain/hogd/pull/3108), branch
`vp/sharding-v2-db-abstraction`) by its
`sharding: checkpoint the tree the benchmark campaign is running` commit. What
made it across:

- `hog-devnode --num-db-shards N`, and the per-shard ramdb slicing.
- The `dbGateway` fan-out, with `HogDevnode.ShardedGatewayServesAccountsOnEveryShard`.
- The shard-identity resolution shared with hogdb.
- The mempool genesis fix — wait for the genesis hog accounting before reporting
  genesis loaded, so the first submitted batch is not discarded for "0 hogs".
- Coverage for where the genesis hog supply lands across shards.

**What did NOT make it across is the viewing-snapshot work — the last two commits
here.** Confirmed on 2026-08-21: `shard_block_id` and
`ShardedViewGatewayViewsWalletsOnEveryShard` do not exist anywhere on the PR
branch. Those two commits touch `database_proxy.{h,cc}`,
`rpc_from_view_gateway.h` and `hog_devnode_test.cc` (~214 lines added across the
pair) and they are the reason to keep this branch.

## The viewing-snapshot gap, and how it was closed here

Handing `ViewGatewayRpc` one db-executor address per shard made `DatabaseProxy`
route reads to the owning shard, but that alone cannot view state a non-primary
shard holds: **each hogdb mints its own viewing-snapshot id**, and
`open_viewing_snapshot()` opened on the primary only. A read routed elsewhere hit
a snapshot that shard had never heard of, retried until its budget ran out, and
came back `ViewGatewayAbandoned`. It was already a `TODO(sharding)` in
`DatabaseProxy`, described as latent because nothing exercised the read path — and
a two-shard `hog-devnode` exercises it.

The fix opens a snapshot on **every** shard and keeps the ids. Callers still see
one `BlockID` (the primary's) and `shard_block_id()` maps it to whatever the
owning shard minted when a read routes there. Keeping the translation inside the
proxy preserves its promise to abstract sharding away from the rest of the
executor, so `ViewState` and the snapshot-scoped reads are untouched.

Two properties worth not regressing:

- **Block execution is unaffected in behaviour and in cost.** Its ids come from
  the scheduler and are identical on every shard, so they are never in the map,
  and the lookup is skipped entirely while no view holds a snapshot — which is
  all of block execution.
- **Failure does not pin snapshots.** A failed open hands back the snapshots
  already taken rather than holding them for the life of the process, and close
  fans out over every shard even after one fails, so one bad shard cannot pin the
  rest.

`ShardedViewGatewayViewsWalletsOnEveryShard` was added disabled by the
commit before, precisely to demonstrate the gap, and is enabled and passing at
this branch's tip.

## What needs doing

1. **Port the two viewing-snapshot commits onto the PR branch.** This is the real
   task. They were written against the pre-rename tree, so the paths move
   (`hogd/hogexec/` → `hogd/hog_exec/`, `hogd/hogdb/` → `hogd/hog_db/`) and
   `rpc_from_view_gateway.h` has changed upstream. Without them, a sharded
   devnode can serve accounts through the db gateway but cannot *view* state on a
   non-primary shard.
2. **Rebase, or don't.** This branch sits on the old base (`97c4f6cc`) and has
   not been rebased onto current `main`. If the answer to (1) is a cherry-pick
   onto the PR branch, it never needs rebasing — read it here and write it there.
3. **Then delete this branch.** It has no reason to exist once (1) is done.

## Building and running it

It is cloneable as it stands: all three submodule pins are on their remotes
(`protocol` `aacb2076` on `vp/shard-card-identity`, `groundhog-go` `a8a2edf` on
`vp/shard-card-identity`, contracts `047b9b2` on `main`).

```sh
./scripts/build.sh -c gcc          # gcc-15, not the default g++ (GCC 16)
./scripts/fetch.sh                 # if the rv32 toolchain check fails
./build/hogd/devnode/hog-devnode --num-db-shards 2
```

See `docs/sharding/BRANCHES.md` on the `vp/sharding-v2-archive` branch for the
full branch map, the protocol-dependency merge order, and the build traps.
