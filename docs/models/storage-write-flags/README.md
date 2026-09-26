# Storage flag readback model

The executor replaces write flags. A token can preserve `ExclusiveLock` itself
if it reads the currently visible flags immediately before each write, including
a write after deletion. No nested call may intervene between that read and write.

This is a bounded TLA+ abstraction of one storage key in one transaction, with
up to two child frames. It does not model balances, lock-manager internals,
parallel transaction commits, syscall encoding or resource charges. A retained
exclusive bit is the precondition used by the real commit-time conflict checks;
the model does not prove those checks. Frame save/restore follows
`InvocationFrameStack` overlay behavior, but is not generated from the C++ code.

## Policies checked

| Policy | Result | Generated / distinct states |
| --- | --- | --- |
| `blind`: overwrite flags | Exclusive then shared write loses lock | 73 / 48 |
| `nnint`: flags only available for live NNInt values | Exclusive delete then recreate loses lock | 84 / 55 |
| `fresh`: generic readback, preserve only Exclusive | Pass: 12 transitions, two nested frames | 844818 / 87894 |
| `stale`: cache readback across nested calls | Child acquires lock; parent overwrites it using old flags | 2374 / 1042 |
| `stickyall`: accumulate every flag | A hypothetical unrelated flag stops using replacement semantics | 95 / 58 |
| `lock`: separate frame-scoped exclusive-lock primitive | Pass: 12 transitions, two nested frames | 724794 / 73798 |

Counterexample configurations allow seven transitions. Passing runs check
exclusive retention, absence of leaked exclusivity and replacement of the
unrelated bit, through successful writes, rejected writes, child success and
failed-child/TryInvoke rollback. Reads and writes are separate in the stale
policy; the fresh policy models the required adjacent read/write pair atomically.
All participating token writes use the selected policy. A raw write deliberately
dropping flags can still drop the lock with the readback design.

`lock` is a comparison design, not implemented code: lock acquisition and the
associated write are modeled as one action, with lock state restored on rollback.
It does not settle lock-only operations, lock charging, or whether locks should
remain after a failed child. Those remain API choices.

## Reproduce

Run `./run-models.sh`. It checks both passing models and the expected invariant
failures; a parser/runtime error does not count as a counterexample. Set
`JAVA_BIN` and `TLC_JAR` to use another local installation. TLC may need permission
to open its local management listener.

Validated with TLC 2.19 ([TLA+ v1.7.4](https://github.com/tlaplus/tlaplus/releases/tag/v1.7.4))
and Java 21. Provide an absolute path to the TLC jar; Java defaults to `java`:

```sh
TLC_JAR=/path/to/tla2tools.jar ./run-models.sh
```

No runtime binaries or tool downloads are included here.
