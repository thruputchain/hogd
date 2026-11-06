# RV32 v6 upgrade

This runtime accepts RV32 v5 and v6 during the rollout. Existing v5 contracts
remain executable. Both use the same 128-byte header. v6 appends an XDR
extension list (`Rv32ArtifactExtensions` in `contract.x`) after initial memory,
which carries the optional contract spec; a v5 artifact reads as v6 with no
extensions. Both formats use the same execution, ABI, gas, bounds, and
section-hash checks.

All validators must support v6 before v6 contracts are deployed. Removing v5
support requires a separate network transition decision that accounts for
stored contracts. New optional metadata kinds need no format or runtime change.
Execution changes still require an explicit compatibility and activation plan.

## Release order

1. Merge and publish the compatible generated protocol bindings.
2. Qualify the combined candidate tools and runtime. Until the producer release
   exists, keep published tools 0.43.0 in `scripts/thruput-tools-version` for
   normal CI and test extension-based v6 separately with the candidate tools.
3. Publish v6-producing tools first. This requires a reviewed, scoped exception
   for checks blocked by the old published runtime or executor. Track those
   failures; unrelated checks must pass.
4. Pin the published tools here, pass runtime CI, and publish compatible hogd,
   x23c, and test_ffi. Release the Rust SDK with that executor.
5. Update the tools runtime catalog and scaffold SDK pin. Remove the producer
   exception and publish an aligned tools release with normal checks passing.
6. Align and publish the TypeScript packages, and qualify the exact published
   CLI/runtime/client combination. Upgrade and verify the network before shipping
   explorer. Preserve network state; any reset needs separate approval.

Runtime validation checks only the extension list's framing: nonzero, strictly
ascending kinds and zero XDR padding. It stores extension bodies, including the
spec, without interpreting them. They are bounded by the total contract size.
Schema-aware tools must apply their own byte, nesting, work, and display limits
before decoding or rendering metadata.
