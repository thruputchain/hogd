# RV32 v6 upgrade

This runtime accepts RV32 v6 only. It has no v5 reader. When
`StorageSnapshotCache` loads a committed v5 contract, translation fails and
execution returns `Unrecoverable`. Existing v5 contracts do not keep working
until their specs are added.

Rebuild and redeploy all contracts on a compatible fresh network. Upgrading
an existing futurenet requires a separately approved network reset. This code
change does not perform or authorize a reset. A persistent-network upgrade
needs a legacy reader and network-config activation before a format change.

## Release order

1. Merge and publish the compatible generated protocol bindings.
2. Publish tools 0.43.0 with the v6 CLI and lowerer. The shared candidate pin in
   `scripts/thruput-tools-version` selects that release. Published 0.42.0 emits
   v5 and is incompatible. The new pin is release preparation, not evidence
   that 0.43.0 has been published or qualified.
3. Rebuild fixtures and pass hosted runtime CI with those published producers.
4. Publish the v6 runtime. Update the tools managed-runtime catalog with the
   real release version and verified asset digests; v0.1.31 is incompatible.
5. Qualify the exact published CLI/runtime pair, then repin SDK integration and
   explorer dependencies. Coordinate network activation and any approved reset
   separately.

Runtime validation treats spec contents as opaque. The spec section is bounded
by the total contract size. Schema-aware tools must apply their own byte,
nesting, work, and display limits before decoding or rendering metadata.
