# hog-devnode

`hog-devnode` is a single process that runs a whole Groundhog chain: the
mempool, executor, database, event server, auth server and indexer all live
inside it. It exists so a developer can get a chain with funded accounts on a
laptop without operating six daemons and a consensus coordinator. It is not a
validator; for that see [`docs/running-a-validator.md`](../../docs/running-a-validator.md).

Run `hog-devnode --help` for the full flag list. This file covers the things
the flag list does not say.

## It listens on loopback, on ports the OS picks

`--listen-host` defaults to `127.0.0.1`, and must be a **numeric** address: a
hostname is not resolved, and a wildcard address (`0.0.0.0`, `::`) is rejected
outright because the readiness manifest has to advertise an address a client
can dial.

Every internal service inside the process binds port 0, so the kernel assigns
it. The four endpoints a client needs are reported in the runtime manifest
(see below) rather than being fixed numbers. `--scheduler-port` pins just the
scheduler port if you need a stable one; `0` (the default) means "let the OS
choose".

## The readiness line

When the node is ready it writes exactly one line of JSON to **stdout** and
flushes it:

```json
{"schemaVersion":2,"status":"ready","manifestPath":"<data-dir>/runtime/runtime-manifest.json","manifestHash":"<sha256 hex>"}
```

Everything else the process says goes to stderr, so stdout is a clean channel
to wait on. The line points at the *runtime manifest*, a larger JSON document
holding the network hash, the protocol token, the producer version, the four
endpoints (`scheduler`, `indexer`, `dbGateway`, `viewGateway`), the bootstrap
account derivation scheme, and the bootstrap accounts with their public keys,
wallet addresses and initial balances. `manifestHash` is the SHA-256 of the
manifest bytes, so a reader can check it did not get a torn file — the manifest
is written to a temp name, fsynced and renamed, so it is never observed
half-written.

`--readiness stdout` is the only sink, and it is the default.

### Compatibility: do not add readiness fields casually

The Groundhog CLI parses this line with a struct declared
`#[serde(deny_unknown_fields)]`
(`groundhog-rpc`, `crates/dev/node-supervisor/src/hogd_dev.rs:151-158`). An
added field is therefore a **breaking** change for every already-released CLI,
not a backwards-compatible one — those versions will refuse a node they used
to accept. The runtime manifest the line points at is parsed more leniently,
so new information is much cheaper to add there. If a readiness field really
is needed, it needs a `schemaVersion` bump and a coordinated CLI release.

## The data directory

Without `--data-dir`, everything lives in a temporary directory that is removed
on exit — a fresh chain every run.

With `--data-dir <dir>`, the directory is created if missing and the node then
holds an exclusive `flock` on `<dir>/.hog-devnode.lock` (mode 0600) for its
whole life. A second `hog-devnode` pointed at the same directory exits with

```
hog-devnode data dir '<dir>' is already locked by another process
```

rather than quietly corrupting the first one's state. The lock is advisory and
released by the kernel when the process dies, so a crashed node does not leave
a stale lock behind.

Inside the directory, the per-service state subdirectories (`runtime`, `ramdb`,
`events`, `indexer`) are created and then chmod'ed to **0700**: they hold dev
account private material and the runtime manifest. Reusing a populated data
directory recovers the committed chain and requires the same bootstrap flags —
mismatched flags are rejected before any state is opened.

## Logging

Logs go to stderr. `HOGD_LOG_FORMAT=json` switches them to newline-delimited
JSON records (`timestamp_ms`, `level`, `target`, `message`, and a `fields`
object with `file` and `line`); anything else, including unset, gives the
human-readable format.

`--log-level <critical|error|warn|info|debug|trace>` sets the runtime level,
**but a release build has a compile-time ceiling of `info`**: levels above it
are compiled out entirely, so they cannot be enabled at runtime. Asking for one
is rejected at startup rather than silently capped:

```
error: Log level 'debug' is unavailable in this build; the maximum verbosity compiled in is 'info'
```

That is not a mistake in your command line — it means you need a debug build
(`scripts/build.sh -t Debug`) to see `debug` or `trace`.

## Shutting down

`SIGTERM` stops the node cleanly. The signal is blocked in every worker thread
so it is always delivered to the main thread, which then unwinds; killing with
`SIGKILL` skips that and leaves the data directory as it was at the last
commit.
