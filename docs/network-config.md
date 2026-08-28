# Network Configuration

Some execution limits are consensus-critical: two nodes that disagree about
them execute the same block differently and fall out of step. `NetworkConfig`
(in `protocol/network_config.x`) is the set of them, and this describes how a
block gets the one it runs under.

The parameters themselves are documented where they are declared. What is not
visible from any one file is the path they travel.

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
- The executor reads individual parameters through `StorageSnapshotCache`,
  which serves them off the same lock-free tip cache as the block height.

`ConfigHashPreimage` pairs it with the storage cost model, and
`ProposalHeader::config_hash` commits to their hash -- so a block records the
rules it was executed under, and a validator holding a different configuration
rejects the proposal instead of diverging silently.

Nothing changes the configuration after genesis yet. Because it is already
per-tip state rather than a compiled-in constant, adding that does not require
touching the sites that enforce it.
