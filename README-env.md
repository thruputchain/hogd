# Environment Variables

Hogd and related processes read the following environment variables.

- HOSTNAME: hostname on which the process can be reached over DNS.
  If not set, defaults to `gethostname()` (validator services only;
  `hog-devnode` uses `--listen-host` and ignores this)
- HOGD_LOG_FORMAT: set to `json` to make `hog-devnode` emit newline-delimited
  JSON logs on stderr instead of human-readable logs.
- TMPDIR: parent directory for `hog-devnode`'s ephemeral node state when
  `--data-dir` is omitted. Defaults to `/tmp`.
- GROUNDHOG_DEVNODE_BINARY: read by the `groundhog` CLI (not by hogd itself)
  to locate the `hog-devnode` executable that `groundhog node` supervises.
