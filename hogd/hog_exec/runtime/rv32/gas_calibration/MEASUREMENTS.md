# Retained calibration evidence

These are historical measurements behind the instruction and syscall schedules,
condensed from the former `docs/rv32/rv32-gas-calibration.md`. They were not rerun
for the documentation cleanup. Run IDs identify the recorded bench jobs; the
source record did not provide a revision or raw JSON for every table. Use fresh
JSON with full provenance for a new pricing decision.

The bench host was `hog10`, an AMD EPYC 9554, using Release builds. Nanoseconds
and relative costs are specific to those builds and that host. The committed
constants remain defined in [`program.h`](../program.h) and
[`gas_cost_table.h`](../../gas_cost_table.h).

## Instructions

Two consecutive saturated, fusion-disabled runs using gcc measured 95 common
cells. Relative differences after median normalization were 0.12% median,
1.22% at p90, and 2.90% maximum. The record did not retain their run IDs.
Representative ratios to the run's median cell were:

| Case | Run 1 | Run 2 |
|---|---:|---:|
| LH pointer chase | 2.648 | 2.651 |
| LW pointer chase | 2.491 | 2.496 |
| DIV chain | 1.979 | 1.980 |
| REM chain | 1.648 | 1.651 |
| BNE taken | 1.416 | 1.416 |

These supported the wide-load tier of 3 gas and branch/division tier of 2 gas.
The original sequential JALR case, reported at 2.607/2.610, did not establish a
changing-target dependency and is superseded by dedicated jump graphs. It must
not be used to validate current jump coverage. The current direct-jump price
has a separate measurement recorded beside `kDirectJumpGas` in `program.h`.

The committed `reference.txt` and `syscall_reference.txt` were regenerated from
run `36165785461`, the nightly at `e2cc27c` (engine `df7566033138ebd2`, source
digest `5222e3822b4ccec7`; JSON in `metrics_history`). The previous references,
engine `d4697729c27d6dbd`, predate two changes: JAL from 1 to 3 gas (#4117) and
the default code limit from 64 to 256 KiB (#4136). Reverting both reproduces the
old fingerprint, so the fusion probe did not change. The old JALR shapes are
replaced by guards on the dedicated jump graphs. Across that run and runs
`35812028325`, `35894280482`, and `36032933141`, the dearest JAL context
measured 3.00–3.01x the baseline against its 3 gas, which `--schedule` would
round up to 4; `kDirectJumpGas` records why 3 was kept. The dearest JALR context
measured 2.69–2.76x against 3, and every changing-target sequence stayed within
budget.

The full-range memory pattern was subsequently corrected: the offset advances
after all eight banks, visiting 512 distinct cache lines over a complete cycle
(32 KiB), or 510 lines in the default saturated body. Previously it repeated
after 64 accesses. Old full-range timings do not measure the corrected workload.
Neither those runs nor the current default-size kernels establish a bound for
the runtime's larger configurable memory limit.

A finer gas unit was considered: across eleven workloads, resolution 1 gave a
4.59x time-per-gas spread, and resolutions 2, 4, 8, and 16 gave 4.03x. The
remaining spread included fused code charged at independent opcode worst cases.
Those results support retaining a simple unit; they are not an absolute runtime
budget or a demonstrated bound across all guest programs.

## Hashing and signatures

Run `33469060485` used a gas unit of 1.6241 ns. It informed the shared host base
of 50 gas, SHA-256 compression cost of 22 gas per block, and signature costs of
17750 gas plus 131 per SHA-512 block. The ECALL instruction charge is additional.

| Case | Measured ns | Charged gas, including ECALL |
|---|---:|---:|
| SHA256, 0 bytes | 117.4 | 73 |
| SHA256, 1024 bytes | 673.2 | 425 |
| SHA256, 32768 bytes | 17798.4 | 11337 |
| SHA256Update, 4096 bytes | 2286.0 | 1485 |
| SHA256Finalize | 114.0 | 73 |
| VerifySignature, 0 bytes | 28635.5 | 17932 |
| VerifySignature, 1024 bytes | 30631.7 | 18980 |
| GetBlockHeight | 69.9 | 51 |

SHA-256 measured 34.53 ns per compression block. No separate hashing intercept
was retained: even zero bytes require a padding block, and the residual after
the crossing and first block was about 2 ns, within the fit's residuals.
Billed-to-measured ratios across the size sweep were the useful check; a high
R-squared alone gave little evidence because the largest input dominated the fit.

Run `33557627276` measured direct `HashBuffers` slot creation: approximately
34.8 ns for Copy and 41.0 ns for Update, flat over tested occupancies 64 through
32768. Its gas unit was 1.6232 ns; `ceil(41.0 / 1.6232)` gave the 26-gas stream
charge. Warm syscall Copy reuses a destination and misses this allocation cost.
The occupancy sweep supports the tested range, not every possible configuration.

ML-DSA-65 verification has its own price: 46592 gas plus 160 per 136-byte
SHAKE256 block, on top of the shared syscall base. Run `39809791818` (hog10,
AMD EPYC 9554, gas unit 1.6219 ns, no margin) measured it:

| Case | Measured ns (entry) | Measured gas | Billed gas |
|---|---:|---:|---:|
| VerifySignatureMlDsa65, 0 bytes | 75891.6 | 46802 | 46802 |
| VerifySignatureMlDsa65, 64 bytes | 75879.8 | 46734 | 46802 |
| VerifySignatureMlDsa65, 1024 bytes | 77949.7 | 48071 | 48082 |

Measured and billed gas include the 50-gas shared base and exclude the ECALL
instruction. The sweep fit 75644.9 ns plus 258.0 ns per block (R-squared 1.0),
which is 46640 gas at zero blocks and 160 per block. Less the shared base that
leaves 46590; the base was raised to 46592 so that no measured length bills
below its measurement. Per gas billed under the ed25519 price the same run put
ML-DSA-65 at 2.53-2.63x the time of ed25519, which this price removes.

`rv32_syscall_report.py` keeps these rows out of the shared syscall numbers,
prints them beside the ed25519 row of the same length, and in `--schedule`
proposes the two ML-DSA constants from each run. Their guards enter
`syscall_reference.txt` the next time it is regenerated with mode=reference;
until then `--check` notes them as ungated.

## Storage

Run `33725560215`, engine `d4697729c27d6dbd`, gas unit 1.6217 ns, recorded five
repetitions and reported means after changes #3771 and #3814:

| Case | ns |
|---|---:|
| Warm entry NNIntGet | 158.4 |
| Warm entry NNIntGetNoLoad | 156.2 |
| Warm entry NNIntSetAdd | 170.6 |
| Warm VM NNIntGet | 182.6 |
| One frame opened and discarded | 124.6 |
| First-touch read, depth 1, 4096 keys | 303.7 |
| First-touch read, depth 16, 4096 keys | 313.7 |
| First touch without load, depth 1, 4096 keys | 216.5 |
| First-touch write, depth 1, 4096 keys | 227.1 |
| Commit per staged item, 4096 items | 51.5 |
| Snapshot-cache miss through local database fixture | 55249 |

The 150-gas storage access charge folds warm access, an allowance for first
touch, and prepaid child commit into each call. Together with the 50-gas shared
host base it bills 200 host gas. This overcharges repeated warm touches but does
not cover the measured first-touch tail at large frame occupancies:

| Keys at depth 1 | Loaded first touch, ns | No-load first touch, ns |
|---|---:|---:|
| 1 | 257 | 219 |
| 256 | 251 | 203 |
| 4096 | 304 | 217 |
| 16384 | 361 | 244 |
| 65536 | 539 | 271 |
| 262144 | 1017 | 386 |

The relevant limit is keys touched, including reads that create item views.
Whether a transaction can reach the expensive tail depends on its gas budget;
these measurements do not establish that the tail is unreachable.

The local database miss measured about 55.2 microseconds with a recorded spread
of 5.4 microseconds. The 37300-gas database charge includes that margin and brings
a loaded cold NNInt read to 37500 host gas. The fixture uses a local socket;
a remote database adds work absent from this measurement. Billing follows the
transaction's first load, independently of whether the shared snapshot cache
happens to contain the key.

## Bytestrings

Run `34001119814` used five repetitions of 0.5 seconds. Reported deviations were
0.03–1.7%. Entry times in nanoseconds were:

| Bytes | Set | Get | GetLen |
|---|---:|---:|---:|
| 0 | 142.2 | 149.4 | 132.2 |
| 32 | 166.4 | 153.4 | 132.5 |
| 256 | 171.2 | 154.1 | 131.9 |
| 1024 | 186.8 | 163.4 | 132.3 |
| 4096 | 282.6 | 202.7 | 132.1 |
| 16384 | 748.4 | 443.6 | 132.2 |
| 32768 | 1383.9 | 757.9 | 132.2 |

GetLen was flat over this range. Set and Get became more expensive per byte at
larger lengths; the largest adjacent band measured approximately 0.0388 and
0.0196 ns/byte respectively. Prices use the most expensive measured band:
13 gas per 512-byte write block and 7 per read block, on top of storage access.
The block size is a pricing constant listed in the gas schedule.

Reads charge bytes copied rather than the offered buffer capacity. This avoids
billing the SDK's reusable scratch capacity when it reads a small item. The
historical billed-to-measured ratios, including each call's measured VM crossing,
were 1.18x–1.87x for Set, 1.34x–1.82x for Get, and approximately 2.2x for GetLen.
These are observations over the measured lengths, not a bound for all inputs.
