# Environment Variables

Hogd and related processes read the following environment variables.

- HOSTNAME: hostname on which the process can be reached over DNS.
  If not set, defaults to `gethostname()`
- HOGD_LOG_FORMAT: set to `json` to make `hog-devnode` emit newline-delimited
  JSON logs on stderr instead of human-readable logs.
- THRUPUT_DEVNODE_BINARY: read by the `thruput` CLI (not by hogd itself)
  to locate the `hog-devnode` executable that `thruput node` supervises.
- HOGD_COORD_MPTCP_LISTEN: set to `1` on `hog-mempool` to enable a Linux
  MPTCP-capable SchedulerCoord RPC listener. Other RPC ports are unchanged;
  ordinary TCP clients are still supported. If MPTCP is unavailable, a warning
  is logged and the standard TCP listener is used.
- HOGD_COORD_PROPOSAL_MPTCP: set to `1` on `chuckd` to request MPTCP only for
  block proposals sent to SchedulerCoord on Linux. One of the usual 20
  connections is dedicated to proposals; the other 19 remain ordinary TCP.
  When the peer or kernel does not support MPTCP, the connection uses TCP.
  A failed proposal is never replayed by the pool; future proposals reconnect.
  Both settings are off by default. Set both for MPTCP negotiation; restarting
  the relevant processes is required to change these settings.
