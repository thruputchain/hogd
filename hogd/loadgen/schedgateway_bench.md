# Scheduler gateway submission baseline

`schedgateway_client_bench` measures the unvetted scheduler-gateway submission
RPC using a separate native client and `hog-devnode`. Each persistent x23
connection sends one transaction per RPC, with one outstanding request. It
exercises native decoding, authentication, and mempool admission without JSON,
HTTP, or gateway batching. This is a focused regression benchmark, not a
production-capacity test or a substitute for the full loadgen stack.

The workload is generated and signed in C++ using hogd's bootstrap accounts and
contract bindings. There are no downloaded transaction fixtures and no runtime
dependency on the Rust gateway or CLI. Building hogd still uses its normal pinned
contract compiler.

## Build and run

From a configured hogd checkout with submodules and build dependencies installed:

```sh
scripts/build.sh -t Release -c gcc -j 16 schedgateway_client_bench hog-devnode
python3 hogd/loadgen/run_schedgateway_bench.py build /tmp/submission-baseline-1
```

The Python standard-library runner starts a fresh node with 100 funded accounts,
eight backend workers, and 2 GiB of RAM database capacity. It saves results,
manifest, host metadata, and logs under the new output directory, then stops the
node and removes its temporary state. Existing output directories are rejected.
Startup and the complete client run have process timeouts.

Useful controls:

```sh
python3 hogd/loadgen/run_schedgateway_bench.py build /tmp/submission-c1 \
  --connections 1 --seconds 5 --warmup 1 --transactions 60000
python3 hogd/loadgen/run_schedgateway_bench.py build /tmp/submission-c4 \
  --connections 4 --seconds 5 --warmup 1 --transactions 60000
```

`--workers` controls backend parallelism (default 8); the client always has eight
x23 workers. `--block-ms` controls proposal cadence (default 500 ms). Missed block
deadlines are skipped, not replayed as a burst. Use an idle host and keep workers,
connections, account count, block cadence, build settings, and host fixed when
comparing revisions. Repeat each configuration at least three times. Retain
`host.json`, `manifest.json`, and `result.json` together. The result identifies
both binaries' commits; uncommitted edits are not described by those hashes.

The runner is also the live smoke check: it requires positive measured throughput,
no client or block-driver errors, and reconciliation of admitted transactions
with block inclusion. It does not install a machine-dependent throughput gate in
ordinary CI. Establish the repeated baseline and its variance on the intended
performance host before choosing a threshold.

## Measurement contract

Transactions transfer one raw native token between successive funded dev wallets.
All signatures and singleton XDR batches are prepared before timing. The client
first submits one transfer, produces a block, and checks its indexer receipt for
successful execution. Each remaining transaction has a unique nonce and is sent
at most once. Exhausting the prepared corpus fails the run; increase
`--transactions` instead of replaying it.

Warmup and measurement share connections and node state. Only successful RPCs
that both start and finish inside the measurement window contribute to
`measuredSuccess` and latency percentiles. Throughput is
`measuredSuccess / seconds`. Calls starting during warmup count as `warmupSuccess`;
measured calls completing after the deadline count as `drainSuccess`. RPC latency
ends at admission, not confirmation. Errors fail the run; they are not retried.

Block production runs concurrently on its own connection. After submission stops,
the client drains outstanding transactions into blocks and checks:

```text
committedTransactions = warmupSuccess + measuredSuccess + drainSuccess
```

The preflight transaction and block are excluded from all these counts. Inclusion
reconciliation does not inspect every transaction's execution result; only the
preflight receipt is checked. `blocks` includes the post-load drain. The drain is
outside the throughput window. A failure before measurement can exit without a
result file; consult `client.log` and `node.log` as well as the exit status.

To use the client directly, start a fresh native `hog-devnode` with
`--dev-accounts 100` and pass its emitted manifest path:

```text
schedgateway_client_bench MANIFEST OUTPUT [CONNECTIONS=4 [SECONDS=5 [WARMUP=1 [TRANSACTIONS=60000 [BLOCK_MS=500]]]]]
```

Use an external process timeout when running directly: individual x23 calls have
no deadline. The client rejects a nonempty mempool or an advanced chain tip; do
not share the node with another block driver or traffic source. Its coordinator
RPCs produce and commit blocks, so use only a disposable local development node.
