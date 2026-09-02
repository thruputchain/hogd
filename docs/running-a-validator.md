# Running a validator

A Groundhog node is not one process. Six binaries from this repository run
together, plus a consensus coordinator that drives them. This describes how
they are wired and what each one needs to start. It is deliberately short on
tuning advice: for the full flag list of any binary, run it with `--help` —
that output is generated from the binary you actually have, and this file is
not.

For a single-process chain on a laptop, use `hog-devnode` instead
([`hogd/devnode/README.md`](../hogd/devnode/README.md)).

## The processes

| Binary | What it does | Dials | Listens for |
| --- | --- | --- | --- |
| `hog-mempool` | The hub: holds pending transactions, nominates, validates and commits blocks | nothing | db, event, executor, auth, coordinator |
| `hog-db` | Durable state | mempool | executor, mempool, auth |
| `hog-exec` | Executes transactions | mempool (optionally db and event directly) | mempool, optional view gateway |
| `hog-event` | Block/event storage and the feed the indexer reads | mempool | executor, mempool, indexer |
| `hog-auth` | Signature and auth-claim verification | mempool | db, mempool |
| `hog-indexer` | Queryable index and block explorer | event server | block explorer |

Every arrow points *at* the mempool or, for the indexer, at the event server.
That is the wiring rule: the mempool binds and waits, everything else connects
inward. `hog-mempool` waits during startup for the db shards it was told to
expect, logging `Waiting for <n> db shards to connect...` until they arrive.

The consensus coordinator (`chuckd`, which forks `scpd`) is a separate program
outside this repository's C++ tree; it connects to the mempool's coordinator
port and drives nomination and commit. See its own `--help`.

## Startup order

Start the mempool first, then the services that dial it, then the coordinator.
That is what `scripts/ci/startup_order_test.sh` calls the `mempool-first`
scenario:

```
hog-mempool → hog-exec → hog-event → hog-indexer → hog-db → hog-auth → coordinator
```

(The script's version of that order also starts `loadgen_server`, a test load
generator that is not part of a validator.)

The order is not sacred: that same script exists precisely to launch these in
deliberately varied orders — `mempool-last`, `event-before-indexer`,
`coordinator-first`, and several `starve-*` scenarios that never start one
component — and to assert that nothing crashes on startup. Read it before
assuming any particular order is required or forbidden. Starting the mempool
first is simply the order with the least waiting.

## Required flags

Each binary rejects a command line missing any of these and prints its usage.
`<ip>` is the address the process binds; `<host:port>` is where it dials
someone else.

```sh
# The hub. --network-hash is 64 hex digits and must be identical on every
# node of a network; a mismatch is how a node ends up talking to nobody.
hog-mempool --bind-ip <ip> \
            --num-db-shards <n> --num-exec-shards <n> \
            --network-hash <64 hex digits> \
            [--db-port <port>] [--event-port <port>] [--executor-port <port>] \
            [--auth-port <port>] [--coordinator-port <port>] \
            [--gateway-port <port>] [--metrics-port <port>]

hog-db --db-path <path> --ram-bytes <bytes> --bind-ip <ip> \
       --coordinator-addr <mempool-ip>:<mempool db port> \
       [--executor-port <port>] [--mempool-port <port>] [--auth-port <port>] \
       [--gateway-port <port>] [--metrics-port <port>]

hog-exec --coordinator-addr <mempool-ip>:<mempool executor port> --bind-ip <ip> \
         [--nthreads <n>] [--mempool-port <port>] [--metrics-port <port>]

hog-event --mempool-addr <mempool-ip>:<mempool event port> \
          --work-dir <path> --bind-ip <ip> \
          [--executor-port <port>] [--indexer-port <port>] \
          [--mempool-port <port>] [--metrics-port <port>]

hog-auth --mempool-addr <mempool-ip>:<mempool auth port> --bind-ip <ip> \
         [--db-port <port>] [--mempool-port <port>] [--metrics-port <port>]

hog-indexer --event-addr <event-ip>:<event indexer port> \
            --db-path <path> --bind-ip <ip> \
            [--block-explorer-port <port>] [--metrics-port <port>]
```

`--coordinator-addr` is optional in `hog-db`'s usage string but is what
attaches it to the mempool; a node without it is not part of a chain.
`hog-indexer --event-port` is accepted and ignored, and says so.

`scripts/ci/startup_order_test.sh` has a complete, working set of these
commands with concrete port numbers, and `scripts/launch-multi.sh` is where
those numbers come from. Both are more likely to be current than this table.

### Data directories

`hog-db --db-path`, `hog-event --work-dir` and `hog-indexer --db-path` each
want their own directory. Do **not** pre-create them: the indexer only builds
a fresh database when its path does not exist, and an empty pre-existing
directory sends it down the open path instead, which fails with "DB Not Found".
Each daemon creates its own.

## Ports, metrics and health

Any port flag left off defaults to 0, which means the OS assigns one. Each
process logs what it got, as `PORT <component>.<service>=<port>` lines, at
startup. That is fine for a test rig and a poor idea for a validator: pin the
ports you need to reach, so a restart does not move them.

Every process serves two unauthenticated HTTP endpoints on its
`--metrics-port`:

- `GET /health` — JSON: process name, pid, last-updated timestamp, status
  (`starting` / `ok` / `error`), current block height.
- `GET /metrics` — Prometheus text format.

**Firewall these.** There is no authentication, no authorization and no TLS on
that port; it is meant for a scrape target on a private network. The same
applies to every inter-process port above — the only port that has any business
being reachable from outside the node's own network is the one your gateway or
coordinator deployment deliberately exposes.

## Network configuration

Some execution limits are consensus-critical: nodes that disagree about them
execute the same block differently and diverge. Where those values come from,
and how they travel from genesis through every commit, is
[`docs/network-config.md`](network-config.md). `--network-hash` must match
across the network for the same reason.
