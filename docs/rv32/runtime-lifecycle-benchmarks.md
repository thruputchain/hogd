# RV32 runtime lifecycle benchmarking

The `lifecycle/` rows in `rv32_syscall_entry_bench` measure costs outside long
instruction loops through the production `Rv32Context`, `Rv32Contract`, and
`Rv32Runtime`. Run on the calibration host, with the same Release tree as the
opcode calibration:

```sh
scripts/rv32_gas_calibrate.sh build/release --out /tmp/gas.json
scripts/rv32_syscall_bench.sh build/release --out /tmp/lifecycle.json \
  -- --benchmark_filter='^lifecycle/'
python3 scripts/rv32_lifecycle_report.py /tmp/lifecycle.json --gas /tmp/gas.json
```

The existing runner uses nine repetitions, CPU affinity, build provenance, and
ASLR suppression where supported. Full nightly/manual calibration runs include
these rows automatically in `rv32-syscall-bench.json`, save them to
`metrics_history`, and print the lifecycle report. No heavy per-PR job is added.
A short functional smoke run can override the benchmark options:

```sh
scripts/rv32_syscall_bench.sh build/release --repetitions 1 \
  -- --benchmark_filter='^lifecycle/' --benchmark_min_time=0.001s
```

| Rows | Timed work |
| --- | --- |
| `PrepareMemory/N` | Verify an artifact, decode/lower code, allocate the zero-extended initial image, destroy the preparation; vary guest memory bytes. |
| `PrepareImage/N` | Same preparation lifecycle, varying stored image bytes at fixed maximum guest memory; includes artifact digest validation. |
| `PrepareCode/N` | Same preparation lifecycle, varying code bytes at 4 KiB memory; padded instructions are valid but never executed. |
| `Setup/N` | Instantiate the production machine from a prepared contract and seed its method ID; destruction excluded. |
| `Teardown/N` | Destroy the machine and recycle its memory slot; construction excluded. |
| `SetupTeardown/N` | Both phases for one machine, using uninterrupted ordinary timing. |
| `SetupTeardownBatch/N` | Both phases for the same batch as the isolated measurements, using manual timing. |
| `TimerFloor` | Empty batch traversal with the same clock and compiler barriers; diagnostic only. |
| `SetupPoolMiss/N` | Setup after explicitly emptying the thread-local free list outside timing; measures allocation misses, not cold CPU caches. |
| `InvokeReturn/N/P` | Copy P calldata bytes into a fresh frame, allocate the runtime, invoke, destroy it, and finish the frame. Nonzero P runs real GetCalldata and Return syscalls, copying through guest memory. |
| `InvokeTrap/N` | Same invocation lifecycle with a deterministic guest trap. |
| `InvokeOutOfGas/N` | Same lifecycle with zero gas, failing at the first instruction run. |

N is bytes. Memory sweeps include 4 KiB, 16 KiB and the VM default maximum
(currently 64 KiB). Ordinary setup, teardown and invocation rows use a warmed memory pool;
only `SetupPoolMiss` explicitly empties it. Teardown measures return to a
non-full pool, not the overflow/free path. Payload sweeps use deterministic,
nonzero calldata and verify the returned bytes before timing. Each timed
invocation checks its outcome and completes its frame, including failures.

These are **inclusive diagnostic costs**, not additive schedule entries:
invocation already includes machine setup/destruction, frame bookkeeping,
guest instructions and syscalls. Preparation includes its eventual destruction
but is outside cached invocations. The report shows median wall time and its
equivalent in the existing opcode gas unit, rounded up. It refuses failed rows
and mismatched source/build/host/engine provenance. It does not propose pinned
prices or silently subtract one noisy timing from another.

The invocation rows start with a prepared contract. They exclude deployment
lookup, the outer Invoke syscall's argument parsing, transaction validation,
DB commit and RCQ/storage operations. They measure runtime lifecycle plus frame
and payload handling, not whole-transaction latency. Calldata entry copying is
performed by the Invocation value copy; the outer Invoke syscall is not timed.
## Timing the isolated phases

The first run exposed pause/resume overhead: 4 KiB teardown alone appeared to
cost 710 ns while uninterrupted setup plus teardown took 65 ns. Those old
isolated rows must not be used for pricing.

Timing version 2 replaces per-operation PauseTiming/ResumeTiming with
steady-clock manual timing around a batch of eight machines (the memory-slot
pool capacity). All eight slots are warmed before sampling. Limiting the batch
to the pool capacity keeps setup on the reuse path and teardown on the recycle
path; a larger batch would benchmark allocator misses and frees instead.
Each repetition takes 100,000 batches (800,000 operations), bounding untimed
work: calibrating teardown to half a second of manual time could otherwise
spend minutes constructing machines. Preparation and cleanup run outside the
explicit clock interval. Timings are
normalized per machine before SetIterationTime, so JSON real_time is already
ns/machine. CPU time includes untimed work and must not be used for these rows.

Use SetupTeardownBatch when comparing isolated phases: its simultaneously live
machines and memory working set match theirs. The one-machine SetupTeardown
row remains an independent ordinary-timer measurement, but its hotter memory
working set can be faster than the batch, especially at 64 KiB per machine.

The report rejects old pause/resume data and requires TimerFloor alongside
isolated rows. It labels measurements below ten times the floor `unresolved`
and withholds a gas estimate. The floor is never subtracted. The displayed
unresolved time includes overhead and is not an isolated operation's price.
Keep TimerFloor in filters (the default `^lifecycle/` includes it). Repeat full
runs on bench hardware before adopting prices.
