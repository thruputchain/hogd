# Public view gas policy

`hog-exec --max-view-gas <gas>` caps each public view execution attempt.
The default is 10,000,000 gas; the CLI requires a positive integer. A smaller
caller budget is preserved, including zero. A larger caller budget is clamped
before execution, and exhausting it returns the existing contract `Fault`
output. This includes callers that use `UINT64_MAX` to request unlimited gas.

This is local RPC service policy, not `NetworkConfig`: it does not change
transaction proposal or validation. Choose a ceiling using representative
view latency on the deployed hardware. Public views bypass scheduler balance
admission, so the caller's gas field cannot be the only resource policy.

Stale snapshots may cause up to three execution attempts. Each gets the
ceiling, so budget capacity for up to three times the configured gas per
request. This bounds metered execution, not all host work or total concurrent
requests. Keep gateway concurrency, request-body, rate, and timeout limits;
review unmetered host work separately. A client timeout alone does not prove
that backend execution was cancelled.
