# Calibration validation and gas units

The full-range memory kernel now advances its within-bank offset after visiting
all eight banks. A complete cycle visits 512 distinct 64-byte cache lines
(32 KiB); the default saturated 510-target body visits 510 lines. The previous
coupled bank/offset pattern repeated after 64 accesses. Run a fresh calibration
before drawing conclusions about the affected memory prices; old measurements
do not describe this workload.

Validation workloads must all build and execute successfully. Each timed run
checks actual consumed gas against the expected prologue, iterations and terminal
charge. At least three repeats are required, and JSON records the successful
sample count and execution verification. Old JSON without that verification is
still readable for historical reports but cannot pass `--check`, `--predict`,
or generate a new reference. Re-run the harness for those operations.

`--check` now enforces individual workload bounds as well as the existing
relative opcode, syscall, pair and workload-spread guards. A workload bound
failure therefore fails the scheduled job directly, even if all workloads slow
down together and their max/min spread stays unchanged. Missing workloads,
missing opcode coverage, partial samples, and zero or non-finite timings fail.

Opcode and syscall proposals share a normalizer and default to one gas per
baseline instruction, matching the committed instruction schedule. Explicit
`--resolution=N` proposals must use the same N for both reports; the syscall
proposal scales the already-billed ECALL instruction credit accordingly. Gas
limits must also be rescaled when adopting a different unit.

These fixes do not change protocol gas prices or either artifact digest.
Any follow-up repricing should use corrected validator-host measurements and
update only the unreleased v4 digest in place; v3 remains frozen.

Approximately 4x workload time-per-gas spread is an initial engineering target,
not a demonstrated universal bound. The existing spread reference and tolerance
are unchanged. A uniform slowdown of the interpreter and workloads together
also requires a hardware-specific absolute time-per-gas budget; these relative
guards do not provide one.

Validation commands:

```sh
python3 -m unittest discover -s scripts -p 'test_rv32*report.py'
cmake --build BUILD_DIR --target rv32_gas_calibration_test rv32_gas_calibrate
ctest --test-dir BUILD_DIR -R '^rv32.gas_calibration.' --output-on-failure
scripts/rv32_gas_check.sh BUILD_DIR --out /tmp/rv32-gas-calibration.json
```

The existing **RV32 Gas Calibration** workflow can run on a selected branch in
`check` mode. Review any changed memory costs or newly detected workload bound
failures before regenerating its reference or changing v4 prices.
