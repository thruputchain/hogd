# Environment Variables

Groundhog binaries read the following environment variables.

## All binaries

- `TMPDIR`: parent directory for temporary state. `hog-devnode` places its
  ephemeral runtime, database, event, and indexer directories here when
  `--data-dir` is omitted. Defaults to `/tmp`.
- `HOGD_LOG_FORMAT`: set to `json` for newline-delimited JSON logs on stderr.
  Any other value (or unset) selects human-readable logs.

## Test and benchmark tooling only

- `HOSTNAME`: hostname on which a multi-host test process can be reached
  over DNS. Read by the multiexec runner and the mempool test coordinator;
  defaults to `gethostname()`. Shipped binaries do not read it.
- `HOG_DEVNODE_TEST_THROW_AFTER_EXECUTOR_START`,
  `HOG_DEVNODE_TEST_THROW_AFTER_GENESIS_LOAD`,
  `HOG_DEVNODE_TEST_WITHHOLD_INDEXER_PULL`: fault-injection hooks used by the
  hog-devnode lifecycle tests. Leave unset.
