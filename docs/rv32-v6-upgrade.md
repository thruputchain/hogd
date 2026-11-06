# RV32 v6 upgrade

This runtime accepts RV32 v5 and v6 during the rollout. Existing v5 contracts
remain executable. v5 uses a 128-byte header with no embedded spec; v6 uses a
136-byte header with an optional spec section. Both formats use the same
execution, ABI, gas, bounds, and section-hash checks.

All validators must support v6 before v6 contracts are deployed. Removing v5
support requires a separate network transition decision that accounts for
stored contracts. Header extensibility is a separate design task.

## Release order

1. Merge and publish the compatible generated protocol bindings.
2. Keep published tools 0.43.0 in `scripts/thruput-tools-version`. Pass runtime
   CI with its v5 artifacts and qualify v6 with the candidate tools.
3. Publish the dual-format hogd, x23c, and matching test_ffi executor. Release
   the Rust SDK with that executor before publishing v6-producing tools.
4. Update the tools managed-runtime catalog with the release version and
   verified asset digests, and update the scaffold SDK pin. Pass normal CI
   and publication checks before publishing tools 0.44.0. No producer-only
   publication exception is needed.
5. Qualify the exact published CLI/runtime pair, then repin TypeScript SDK
   integration and explorer dependencies. Upgrade and verify devnet before
   shipping explorer. Preserve network state; any reset needs separate approval.

Runtime validation treats spec contents as opaque. The spec section is bounded
by the total contract size. Schema-aware tools must apply their own byte,
nesting, work, and display limits before decoding or rendering metadata.
