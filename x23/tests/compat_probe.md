# Native ONC RPC compatibility probe

`compat_probe.cc` demonstrates on one live socket that x23 serves two adjacent
versions of one program concurrently and returns distinct ONC RPC negotiation,
procedure, and argument-decoding failures.

The generated x23 client throws `x23::XdrCallFailed` for a non-successful RPC
reply. The exception's public `m_` member is the decoded `rpc_msg`; callers can
read a `PROG_MISMATCH` range programmatically from:

```c++
e.m_.body.rbody().areply().reply_data.mismatch_info().low
e.m_.body.rbody().areply().reply_data.mismatch_info().high
```

The exception message omits the range, so callers must catch
`XdrCallFailed` and inspect `m_`. No x23 change is required to obtain low/high.
If callers need a stable API that does not expose generated wire internals,
x23 would need accessors on `XdrCallFailed` (for example, an accepted status
and an optional program-version range).
