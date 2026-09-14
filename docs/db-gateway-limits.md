# Database gateway read limits

The `hog-db` read gateway has two local service settings:

| Flag | Default | Valid values |
|---|---:|---|
| `--gateway-max-items` | 1024 | Positive integer |
| `--gateway-max-response-bytes` | 8388608 (8 MiB) | 16 through the default RPC message cap minus 1024 bytes |

The response budget measures the serialized `DbGatewayResult`, including the
outer discriminant, height, vector length, item discriminants, and XDR padding.
The transport allowance leaves space for RPC framing. The item limit is checked
before reserving result storage or reading rows. Byte accounting is incremental:
the handler retains no successful batch above the ceiling, although it can
materialize one additional item to determine that the request exceeds it.
Duplicates count independently because the response preserves input order.

An over-limit request fails as a whole with `kRPCUnrecoverable`; the current
protocol has no dedicated resource-limit error. There is no successful partial
result. The public gateway should split oversized aggregation batches and avoid
retrying an unchanged over-limit batch. Separate batches need not observe the
same height, so clients must not present them as one atomic snapshot.

These settings do not change consensus or transaction execution. They bound
handler work and retained results, not the earlier XDR request decode or queued
responses to slow readers. Keep request-size, concurrency, rate, and transport
backpressure policies at the gateway as well.
