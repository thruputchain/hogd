# RV32 v6 upgrade

This runtime accepts RV32 v5 and v6 during the rollout. Existing v5 contracts
remain executable. Both use the same 128-byte header. v6 appends an XDR
extension list (`Rv32ArtifactExtensions` in `contract.x`) after initial memory,
which carries the optional contract spec; a v5 artifact reads as v6 with no
extensions. Both formats use the same execution, ABI, gas, bounds, and
section-hash checks.

All validators must support v6 before v6 contracts are deployed. Removing v5
support requires a separate network transition decision that accounts for
stored contracts. New extension kinds need no format or runtime change.

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

Runtime validation checks only the extension list's framing: nonzero, strictly
ascending kinds and zero XDR padding. It stores extension bodies, including the
spec, without interpreting them. They are bounded by the total contract size.
Schema-aware tools must apply their own byte, nesting, work, and display limits
before decoding or rendering metadata.
