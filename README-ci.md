# CI Debug information

- Core dumps are saved to `../core_dumps`
	- On the hogN machines: this is `$WORK_DIR/hogd/core_dumps`.  Usually `$WORK_DIR=/home/hogci/actions-runner/_work`, although on `hog11` this is `/home/hogci/general_runner/_work`

# CI runner setup instructions

## Private source authentication

CI calls `.github/actions/private-sources` after checking out hogd with
`persist-credentials: false`, before initializing submodules or fetching Cargo
dependencies. Set repository variable `COMPONENT_READER_APP_ID` and secret
`COMPONENT_READER_PRIVATE_KEY` for the `thruput-component-reader` GitHub App.
Its installation needs Contents: read on hogd, chuckd, hogx, thruput-protocol,
thruput-protocol-go, thruput-protocol-rs, lib-go, thruput-go,
thruput-integration-test-contracts, scp, and thruput-sdk-rs. The hackathon
workflow additionally requests Contents: read on thruput-tools to build its
RPC gateway; other callers keep the base repository scope.

The action maps legacy and canonical repository URLs to authenticated HTTPS.
It creates a private config under `RUNNER_TEMP` and exports `GIT_CONFIG_GLOBAL`,
so neither the App token nor its rewrites enter the runner's `~/.gitconfig`.
The job-local config includes the existing XDG Git config and `~/.gitconfig`
before adding token rewrites, preserving deploy-key rewrites for remaining repos.
Git chooses the longest matching `insteadOf` prefix regardless of file order.
Repository-specific token rules beat organization-wide deploy-key rules; an
included rule with an identical or longer repository prefix can take precedence
and must be removed or narrowed on the worker/runner for transferred repos.
Each caller removes the config in an `always()` cleanup step; the token action
revokes the installation token at job completion. Existing pre-checkout
`.gitconfig` cleanup steps still remove stale runner configuration. Cargo uses
the Git CLI. Go module paths remain unchanged; this tree has no go.mod/go.work
or workflow GOPRIVATE/GOFLAGS settings to migrate.

Deploy-key inputs are retained only for repositories still in groundhogchain:
abort_rs (`HOGD_TO_ABORT_RS_KEY`), bcs (`HOGD_TO_BCS_ACCESS_KEY`),
metrics_history (`HOGD_TO_METRICS_HISTORY_KEY`), serde_arrays
(`HOGD_TO_SERDE_ARRAYS`) and xdr_common (`HOGD_TO_XDR_COMMON_ACCESS_KEY`).
xdrs and xdr_rpc moved to thruputchain; hogd gets them from the registry, so it needs no
key or token for them. metrics_history is fetched
and written by the metrics scripts. The other four keys preserve access to
nested private sources: their manifests are unavailable in an uninitialized
checkout, so qualify their continued necessity in CI before deleting secrets.
The obsolete bcs_rs key and all transferred-repository keys, including the hogd
self key, are no longer referenced. Do not delete stored secrets until CI is
qualified.

Release publication and protocol-rs workflow dispatch keep their existing
write credentials. The read-only component token never rewrites metrics_history.
The hog_sched workflows pass the minted token as the `git_token` template value
using `--key=git_token`. Every fetching worker job creates its own private
`GIT_CONFIG_GLOBAL` file, bootstraps the hogd checkout via an HTTPS rewrite,
and runs `.github/actions/private-sources/git-config.sh` after checkout for
the same legacy and canonical URL map as the composite. Stored remote URLs
remain token-free. Worker EXIT traps remove the config; the existing SSH agent
still serves groundhogchain repositories. Cargo uses the Git CLI on workers.

For manual launches, set `GIT_TOKEN` to a private-source read token before running
`scripts/schedules/launch.sh <xid> <schedule_toml>`. The launcher passes it as
`git_token`. An unset/empty token or a missing template value (`<no value>`)
skips token rewrites and uses the existing SSH authentication.

Installation tokens expire after one hour (and are revoked when the runner job
finishes). Long builds can delay subsequent Cargo/Go dependency fetches beyond
that window, particularly sequential ccache warmup variants and Go service
builds after C++ builds in the multi-node/combined metrics lanes. Those late
fetches need qualification; workers do not renew tokens.

## Runner provisioning
- Create the hogci home directory on the machine in question. You should create it at `/home/local/hogci` and then create a symlink from `/home/hogci` to `/home/local/hogci`: `ln -s /home/hogci /home/local/hogci`.
- Log in as the `hogci` user before completing the remaining steps in this file. You should add you SSH public key to `~hogci/.ssh/authorized_keys` and then ssh in as `hogci`.
- Follow the instructions to clone and register the runner as they appear on Github.
	- Make sure to add the runner overall to the groundhogchain organization, not an individual repository
	- Make sure that when you register the runner, register it with any tags you want to add (this can be done later
	by deregistering and re-adding the runner).  Probably should at least add the machine name as a tag.
- Copy the svc_nosudo.sh script from an existing runner, and edit the filenames/machine names at the top of that file
to the appropriate values for that machine.
- Edit the `bin/actions.runner.service.template` file: At the bottom, replate `multi-user.target` with `default.target`.
  This makes the machine relaunch the service after rebooting.
- `./svc_nosudo.sh install` (may need to mkdir the appropriate ~/config subdirectories) 
- Add github to the ssh known-hosts (otherwise the runner will fail to clone anything from github). You can do this by running `ssh git@github.com` and then choosing to trust the remote host.
- Create a rust installation for the hogci account (much of the config for rust is kept locally)
	- `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
	- Follow the default instructions there
	- `cargo install --force cbindgen`
	- You do not need to add the ./cargo/env to a .bashrc for the runner; the CI scripts add, if necessary, .cargo/bin to the path
	- The CI scripts will, where relevant, fetch any dependencies or toolchain components
- Allow services to run after the shell closes.  From a sudoer account, `sudo loginctl enable-linger hogci`
- `./svc_nosudo.sh start`

## Hackathon workload: one validator

The manual **Hackathon workload (single validator)** workflow runs the existing
single-validator topology on hog4–hog11. It adds the public JSON-RPC gateway on
hog4 and a concurrent replay client on hog7. The schedule fetches a pinned
`thruput-tools` revision on hog4 and compiles `hog-gateway` in Release mode,
with heap profiling enabled, before building hogd. Its Rust toolchain and Cargo
lockfile come from that source checkout. No preinstalled gateway or gateway
workflow inputs are required. The pins live beside the build commands in
`scripts/schedules/hackathon.toml`.
Use exclusively allocated benchmark hosts with matching control servers.
Launch it from **Actions → Hackathon workload (single validator) → Run workflow**
after the workflow has been added to the default branch.
The job prints reports in its logs and uploads a `hackathon-results-<attempt>`
artifact to the GitHub Actions run. It contains `hackathon-results.tar.gz`, with
results and signed corpora retained even after a replay fails. A copy also stays
under `WORK_DIR` and, when set, the worker's `HOG_DUMP_DIR`; the exact paths
appear in the job log.
This job does not publish to `metrics_history` and does not load its deploy key.
The gateway source and qualified hogd protocol revision are pinned together.
The schedule fails before building if the protocol pin no longer matches hogd;
update both pins after checking wire compatibility. The qualified pin uses
`thruput-xdr` 0.55.0 bindings, whose wire definitions match this checkout. CI
supplies hogd's protocol fingerprint to the gateway automatically, and normal
backend handshakes still run. The results retain `gateway-build.json` with the
source revision, binary and Cargo lockfile checksums, Rust compiler, build
profile, features, and protocol fingerprint. Each replay also verifies the
actual gateway binary's checksum and records the fingerprint in its identity.

The CI workload is synthetic: 1,000 signed users, 500 distinct application artifacts,
five wallet artifacts, one/two-signature accounts, deployments, and auth-data
updates. Application families exercise token-like distributions, counters,
records, hashing, and cross-contract calls/events. These are benchmark fixtures,
not implementations of complete production applications. 400 apps are deployed
before measurement; 100 arrive during traffic. Unready deployment dependencies
are counted as blocked arrivals. The shared-code control retains the application
families and arrival schedule but reuses five app artifacts and one wallet
artifact; variant-specific operation costs can differ, so this control alone
cannot prove a cache effect.

Compilation and signing happen before replay. The saved corpus contains the
wallet-only genesis ceremony, setup transactions, contract identities, signed
transactions, and a deterministic arrival schedule. Fixture keys are disposable
and deterministically derived, never devnet keys. The corpus is for a **fresh**
benchmark chain; replaying it on an existing chain is unsupported.

The CI schedule warms up for 30 seconds at 1,000 calls/second, then runs
60-second stages at 1,000, 5,000, 10,000 and 25,000 calls/second. Each stage
contains a five-second fivefold burst (up to 125,000 calls/second). Deployments
are additional traffic. The workflow allows 26,624 concurrent submissions and uses
16 receipt-polling workers. The harness writes and retains `gateway.toml`,
setting the gateway connection cap to submission slots + receipt workers + 128
(26,768 with the workflow defaults), so its default 2,000-connection cap does not stop the
load before ingestion. HTTP errors are counted separately from transport errors;
HTTP 429 rejections are not repeatedly polled for nonexistent receipts.
It records arrivals more than 100 ms late and drains
receipts for 120 seconds.
It does not automatically retry ambiguous submissions. Each corpus runs through
the RPC gateway three times, with fresh state. The workflow repeats those runs
for the shared-code control, giving six measured runs in total.

Workflow inputs `users`, `rate_multiplier`, `outstanding`, `receipt_workers`,
and `repeats` override these defaults. The shell wrapper accepts the corresponding
`HACKATHON_USERS`, `HACKATHON_RATE_MULTIPLIER`, `HACKATHON_OUTSTANDING`,
`HACKATHON_RECEIPT_WORKERS`, and `HACKATHON_REPEATS` environment variables.
Direct shell and standalone harness invocations retain 16,384 submission slots
unless `HACKATHON_OUTSTANDING` or `--outstanding` is supplied.
Use `users=100` and `rate_multiplier=1` for the earlier low-load corpus; the
standalone corpus builder retains these lower defaults for local use. The
multiplier applies before `--rate-divisor` and is saved in the manifest.

Inputs `gateway_batch_shards` and `gateway_inflight_batches` control submission
batching inside the RPC gateway. The workflow defaults to four shards and four
concurrent scheduler batches per shard. The standalone harness and shell wrapper
retain one shard and four batches per shard unless explicitly configured. The shell wrapper accepts
`HACKATHON_GATEWAY_BATCH_SHARDS` and `HACKATHON_GATEWAY_INFLIGHT_BATCHES`;
the standalone harness accepts `--gateway-batch-shards` and
`--gateway-inflight-batches`. Both values must be between 1 and 128. Each repeat
retains the effective `gateway.toml`, and `identity.json` records these settings.
Compare gateway and backend latency when tuning them: additional client slots
can increase gateway queueing without increasing validator load.

`gateway_workers` sets the gateway's Tokio runtime worker count through
`TOKIO_WORKER_THREADS`. The workflow defaults to eight workers. Zero leaves
automatic sizing in effect and remains the standalone harness and wrapper default;
values 1 through 128 set an explicit count. This only configures the gateway,
and `identity.json` records the requested value as `gatewayRuntimeWorkers`.
The wrapper accepts `HACKATHON_GATEWAY_WORKERS` and the standalone harness
accepts `--gateway-workers`.

For subsequent capacity probes with the same workload, `reuse_corpus=true`
restores hog10's retained `/tmp/hogdump-hogci/hackathon-results.tar.gz` instead of
recompiling and signing the fixtures. Source changes must be confined to the
hackathon harness, and the protocol, users, rates and fixture settings must match.
The allowlist includes replay scheduling and transport files so client-capacity
experiments can use identical signed transactions. Generator, contract, bootstrap,
validator and dependency changes still require regeneration. Each run records its
current replay binary hash and protocol setting separately from corpus provenance.
CI's generated registry-configuration patch must also match exactly.
Both signed transaction checksums are checked before either corpus is installed.
Each replay still starts a fresh chain. Results retain `corpus-source.json` with
the source identities, archive checksum and workload parameters. The shell
equivalent is `HACKATHON_REUSE_CORPUS=1`; incompatible or missing caches fail
before replacing the retained archive. Disable reuse to regenerate the workload.

Reports show offered, submitted, admitted and observed-successful TX/s separately,
plus client drops and admitted transactions without observed receipts at each
stage boundary. Completion throughput includes receipts for earlier-stage
transactions. A plateau with missed arrivals or exhausted submission slots can
be a client limit; a run that keeps up with all offered traffic does not establish
maximum capacity. Inspect service metrics and receipt lag alongside throughput.

The benchmark implementation lives in `hogd/loadgen/hackathon`: the corpus builder,
contract fixtures, Python harness and reports, tests, and standalone Go replay tool.
The CI launcher and host schedule remain in `scripts/ci` and `scripts/schedules`.
The standalone `hackathon-replay` tool lives in `hogd/loadgen/hackathon/replay`.
Its `bootstrap` command supplies only the saved genesis configuration and ceremony
over chuckd's existing loadgen interface, keeping the internal work stream open
without supplying application transactions. The stock `bench coordinator_loadgen`
command runs consensus. The tool's `run` command submits all application setup,
deployments, calls, and authentication updates through the RPC gateway. Setup
batches must commit successfully before the measured arrival clock starts.
Submissions use up to 16 independent HTTP connection pools to avoid serializing
connection reuse at large client windows. Their combined connection limit equals
`outstanding`; receipt polling and telemetry reserve another `receipt_workers + 32`
connections. The global outstanding limit and offered arrival schedule still apply.
The `submission_http2` workflow input (or replay's
`--submission-http2` flag) multiplexes measured transaction submissions over HTTP/2,
including cleartext HTTP/2 for the CI gateway URL. The workflow enables it by
default; standalone replay and shell invocations require opting in. Setup,
receipt polling and telemetry use the separate HTTP/1 pool. The selected protocol
is recorded in the run configuration and workload identity. HTTP/2 reduces TCP
connection demand; gateway request limits and bounded ingress queues still apply.
This requires no chuckd changes or submodule updates.

Run with the schedule’s CI services and gateway already built and their control
servers ready (the gateway build metadata is read from hog4):

```sh
export BUILD_DIR=/absolute/path/to/release
export WORK_DIR=/absolute/path/to/new-work-directory
scripts/ci/hackathon_bench.sh
```

For a local correctness smoke, build the normal service targets with
`scripts/build.sh`, build `chuckd.dev` and `scpd.dev` using the repository scripts,
and build the standalone replay tool and a smaller corpus:

```sh
GOWORK=off go -C hogd/loadgen/hackathon/replay build -o /tmp/codex-build/hackathon-replay .
python3 hogd/loadgen/hackathon/build_contracts.py --output /tmp/hackathon-artifacts --apps 5 --wallets 5
scripts/build.sh -d /tmp/codex-build -t Release \
  -x -DHACKATHON_ARTIFACT_DIR=/tmp/hackathon-artifacts hackathon_corpus
/tmp/codex-build/hogd/loadgen/hackathon/hackathon_corpus \
  --artifacts /tmp/hackathon-artifacts --out /tmp/hackathon-corpus \
  --apps 5 --users 10 --wallets 5 --stage-seconds 2 --warmup-seconds 1 --rate-divisor 100
python3 hogd/loadgen/hackathon/cluster.py --local --build /tmp/codex-build \
  --scpd /absolute/path/to/scpd.dev --gateway /absolute/path/to/hog-gateway \
  --gateway-sha256 <expected-sha256> --corpus /tmp/hackathon-corpus \
  --output /tmp/hackathon-results --repeats 1
```

The corpus builder requires the same installed Groundhog CLI (0.26.0), Rust
compiler and cached Cargo dependencies as `built_contracts/build_risc.sh`.
It creates its build workspace beneath `--output`, never in the source tree.
Normal builds do not compile the synthetic corpus. `--shared-code 1` on the C++
generator selects the control; preserve the same seed and schedule options.
Local mode still runs coordinator/SCP and the individual services with exactly
one validator; it is a correctness smoke, not a capacity comparison with CI.
Ports match the single-validator CI setup, so they must be available locally.

Each `gateway-N` repeat directory retains service logs, `replay/results.jsonl`,
metric snapshots, generator resource use, `summary.json`, and `report.md`.
`replay/setup.jsonl` records application setup submissions through the gateway.
The full signed corpus remains in the input directory. Results distinguish
admission, execution outcomes, dependency-blocked work, missed arrivals, and unresolved
transactions. Receipt latency includes polling and indexer delay.
Service exits, setup faults, committed execution faults, and missing required
diagnostics fail the run; overload outcomes remain recorded benchmark data.

Lock wait/hold histograms, authentication timings, and sampled same-host
ingestion/block intervals are always enabled. Logging and measurement have a cost.
A single validator may not exercise validation RPCs, which the report marks as
unexercised. Block overlap is correlation: inspect auth duration/retries and
commit/scheduler lock wait distributions before attributing latency to locks.
No latency regression threshold or automatic recurring schedule is installed.

Focused checks:

```sh
HACKATHON_CORPUS_BINARY=/tmp/codex-build/hogd/loadgen/hackathon/hackathon_corpus \
HACKATHON_ARTIFACTS=/tmp/hackathon-artifacts \
python3 -m unittest discover -s hogd/loadgen/hackathon -p 'test_*.py'
GOWORK=off go -C hogd/loadgen/hackathon/replay test -race ./...
```
