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
| `SetupTeardown/N` | Both phases together, with no per-iteration timer pauses. |
| `SetupPoolMiss/N` | Setup after explicitly emptying the thread-local free list outside timing; measures allocation misses, not cold CPU caches. |
| `InvokeReturn/N/P` | Copy P calldata bytes into a fresh frame, allocate the runtime, invoke, destroy it, and finish the frame. Nonzero P runs real GetCalldata and Return syscalls, copying through guest memory. |
| `InvokeTrap/N` | Same invocation lifecycle with a deterministic guest trap. |
| `InvokeOutOfGas/N` | Same lifecycle with zero gas, failing at the first instruction run. |

N is bytes. Memory sweeps include 4 KiB, 16 KiB and the VM default maximum (currently 64 KiB). Ordinary setup, teardown and invocation rows use a warmed memory pool;
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
Tiny isolated teardown timings are particularly sensitive to timer overhead;
use the uninterrupted SetupTeardown and Invoke rows as the primary costs, and
repeat full runs on bench hardware before adopting prices.
