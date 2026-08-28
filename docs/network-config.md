# Network Configuration

**Status:** Pre-production contract

Some execution limits are consensus-critical: two nodes that disagree about
them execute the same block differently and fall out of step. `NetworkConfig`
(in `protocol/network_config.x`) is the set of them, and this describes how a
block gets the one it runs under.

The parameters themselves are documented where they are declared. What is not
visible from any one file is the path they travel.

## How a block reaches its configuration

`GenesisConfiguration::network_config` sets what block 0 runs under. The
protocol names no value for any parameter, so whoever creates a chain chooses:
`GenesisConfigurationBuilder::build` takes the configuration as an argument,
and `stock_network_config()` holds the values this repository's own dev and
test chains use.

From there it rides the `DbTip`, exactly as `default_wallet` does:

- `dbsched_load_genesis_configuration` installs it via
  `RPCContext::set_network_config`; the genesis commit's `write_dbtip`
  persists it.
- `RPCContext::update_dbtip` mutates the tip in place, so every later commit
  carries the field forward.
- The scheduler copies it from the committed parent tip when it builds the
  nominated or validated tip (`rpc_from_coord.cc`).
- `SystemCallHandler` reads it off the tip before admitting the first frame
  and memoizes it, so the syscall paths that enforce it have no tip read left
  to fail on.

`ConfigHashPreimage` pairs it with the storage cost model, and
`ProposalHeader::config_hash` commits to their hash — so a block records the
rules it was executed under, and a validator holding a different configuration
rejects the proposal instead of diverging silently.

## Changing parameters at a block boundary

`ConfigUpgrade` names one parameter; `apply_config_upgrade` produces the
configuration that results. `ConsensusValue::upgrades` already carries up to
six per block, which is where a change gets its reach.

The pieces an upgrade still needs, none of which exist yet:

1. **Selection.** Nothing populates `ConsensusValue::upgrades`, and the
   scheduler rejects a value carrying one.
2. **Application.** The committing block would decode its upgrades, apply them
   to the parent tip's configuration, and write the result into the new tip
   instead of copying the parent's forward. `update_dbtip` carries the field
   in place today, so that is the line that would change.
3. **Commitment.** The header's config hash must be computed from the
   post-block configuration, which is what `dbsched_close_snapshot` already
   reports — so a block that changes parameters commits to what it leaves
   behind, and its own execution still runs under the parent's.

Because the configuration is already per-tip state rather than a compiled-in
constant, adding those three does not require touching the sites that enforce
it.
