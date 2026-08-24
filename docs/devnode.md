# hog-devnode Operator Guide

`hog-devnode` is the single-process local Groundhog development node. It
runs the scheduler, database, auth, event, executor, and indexer
subsystems in one binary, bootstraps a dev wallet plus deterministic dev
accounts, and advertises its service endpoints through a runtime
manifest that downstream tools (`groundhog node`, `hog-gateway
--runtime-manifest`, `groundhog-bench`) consume.

## Quickstart

```sh
# Ephemeral node: state lives in temp directories and is removed on exit.
hog-devnode

# Persistent node: state survives clean stop/restart cycles.
hog-devnode --data-dir ~/.groundhog/devnode

# Low-memory machines (e.g. WSL): shrink the RAM database reservation.
hog-devnode --ramdb-bytes 2147483648
```

On success the node prints exactly one readiness JSON line to stdout
(see below) and then serves until it receives `SIGTERM`. All operational
logs go to stderr; stdout is reserved for the readiness line.

## Flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--listen-host <ip>` | `127.0.0.1` | Host/interface for the internal x23 services. Must be a numeric IP (`localhost` is accepted and mapped to `127.0.0.1`); wildcard addresses (`0.0.0.0`, `::`) are rejected because the address is also advertised in the runtime manifest. |
| `--scheduler-port <port>` | `0` | SchedulerCoord port; `0` asks the OS for a free port. All other service ports are always OS-assigned and published via the runtime manifest. |
| `--data-dir <dir>` | unset | Persist node state under this directory. Reusing a populated directory recovers the committed chain and requires the same bootstrap flags. Omit for an ephemeral node. |
| `--readiness stdout` | `stdout` | Readiness sink. `stdout` is currently the only value. |
| `--contract-artifacts-dir <dir>` | unset | Search this directory before the default contract artifact locations. May be repeated. |
| `--dev-wallet-profile <name>` | `hog-devnode-default` | Built-in dev wallet profile. |
| `--bootstrap-manifest <path>` | unset | Bootstrap intent manifest. Conflicts with the dev-account and wallet shorthand flags below. |
| `--dev-accounts <count>` | `0` | Deterministic user accounts to fund, excluding dev-0. |
| `--dev-account-seed <seed>` | `groundhog-dev-accounts` | Seed phrase for deterministic dev accounts. |
| `--dev-account-tokens <tokens>` | `1000000000` | Initial storage-token balance per user account. dev-0 is always the fixed 10000000000000-raw local faucet. |
| `--log-level <level>` | build-dependent | `critical`, `error`, `warn`, `info`, `debug`, or `trace`. Levels stripped from the build are rejected rather than silently capped. |
| `--ramdb-bytes <bytes>` | `16106127360` (15 GiB) | RAM database capacity to reserve at startup. Lower it on low-memory systems. |
| `--version` | | Print version and exit. |
| `-h`, `--help` | | Print usage and exit. |

Environment:

| Variable | Meaning |
| --- | --- |
| `HOGD_LOG_FORMAT=json` | Emit newline-delimited JSON logs to stderr instead of human-readable logs. |

A flag given without its value fails with `error: <flag> requires a
value`; an unknown flag fails with `error: Unrecognized option: <arg>`.
Both print usage to stderr and exit non-zero.

## Readiness contract

When initialization completes, the node emits exactly one JSON line on
stdout and flushes it:

```json
{"schemaVersion":2,"status":"ready","manifestPath":"/path/to/runtime/runtime-manifest.json","manifestHash":"<hex sha-256 of the manifest bytes>"}
```

- `schemaVersion` — readiness/manifest schema version (currently `2`).
- `status` — always `"ready"`; failures before readiness never print to
  stdout. They log to stderr as `hog-devnode failed before readiness:
  <cause>` and the process exits non-zero.
- `manifestPath` — absolute path of the runtime manifest, written
  atomically (temp file + rename + directory fsync) before the
  readiness line is printed.
- `manifestHash` — lowercase hex SHA-256 of the manifest file bytes, so
  supervisors can verify they read the manifest this process wrote.

Supervisors should read stdout line-by-line until this line appears,
then load the manifest. Nothing else is ever written to stdout.

## Runtime manifest and endpoint map

The manifest (`runtime-manifest.json`, schema version 2, `kind:
"hogd-dev-runtime"`) carries the chain identity (`networkHash`,
`protocol`, `genesisHash`), `chainState` (`"fresh"` or `"recovered"`),
producer version info, bootstrap account metadata, dev wallet
entrypoints, the storage cost model, and the endpoint map. Endpoints
are `host:port` strings using the bind host:

| Manifest field | Backend | Serves |
| --- | --- | --- |
| `endpoints.scheduler` | mempool scheduler (`SchedGateway1`) | Transaction submission, tip queries, in-flight tx lookup, pending-tx count. |
| `endpoints.indexer` | indexer (`IndexerBlockExplorer1`) | Receipts, events, transaction and block queries. |
| `endpoints.dbGateway` | hog_db (`DbGateway1`) | Committed storage reads. |
| `endpoints.viewGateway` | view gateway | Read-only contract view execution. |

These are internal x23 RPC endpoints, not JSON-RPC: point
`hog-gateway --runtime-manifest <manifestPath>` at the manifest to get
the public JSON-RPC surface. See `docs/gateway/runtime-manifest.md` in
the groundhog-rpc repository for the consumer-side quickstart.

## --data-dir lifecycle

Without `--data-dir` the node is ephemeral: all state lives in temp
directories that are removed at shutdown.

With `--data-dir <dir>`:

- The directory is created if missing. An exclusive `flock` is taken on
  `<dir>/.hog-devnode.lock`; a second node pointed at the same
  directory fails fast with "already locked by another process".
- State is laid out in subdirectories: `runtime/` (runtime manifest),
  `ramdb/` (database log), `events/` (durable event outbox), and
  `indexer/` (indexer RocksDB).
- First startup writes `<dir>/hog-devnode-data.json` recording the
  bootstrap intent (`--dev-accounts`, `--dev-account-seed`,
  `--dev-account-tokens`, `--dev-wallet-profile`).
- Restarting against a populated directory recovers the committed chain
  (`chainState: "recovered"` in the manifest) and re-checks the
  bootstrap flags against the recorded metadata. A mismatch in any of
  the four flags above, or in the network hash, fails startup with an
  error naming the offending flag. To change bootstrap parameters,
  start from a fresh directory.
- Ports are not persisted: every startup binds fresh ports and rewrites
  the runtime manifest, so consumers must re-read the manifest after a
  restart.

## Troubleshooting

- **`Couldn't mmap segment ... Cannot allocate memory`** — the RAM
  database reserves `--ramdb-bytes` (default 15 GiB) of anonymous
  mappings at startup. On machines (or WSL instances / containers) with
  less available memory the mmap fails with `ENOMEM`. Lower
  `--ramdb-bytes`; a couple of GiB is plenty for local development.
- **`Couldn't mmap huge pages`** — a warning, not an error. The node
  falls back to standard pages; reserve huge pages only if you want the
  performance.
- **`data dir '<dir>' is already locked by another process`** — another
  hog-devnode (possibly supervised by `groundhog node`) is running
  against the same `--data-dir`. Stop it or pick another directory.
- **`data dir was initialized with different <flag>`** — the directory
  was bootstrapped with different dev-account/wallet flags. Rerun with
  the recorded flags or use a fresh directory.
- **`recovered chain network hash ... does not match requested network
  hash`** — the directory belongs to a chain bootstrapped from a
  different (or no) `--bootstrap-manifest`. Use the matching manifest
  or a fresh directory.
- **`Invalid listen host`** — `--listen-host` must be a numeric,
  non-wildcard IP (or `localhost`). DNS names are not resolved.
- **No readiness line** — the node failed before readiness; the cause
  is on stderr (`hog-devnode failed before readiness: ...`). Set
  `HOGD_LOG_FORMAT=json` for machine-readable stderr.
