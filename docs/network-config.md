# Network Configuration

Some execution limits are consensus-critical: two nodes that disagree about
them execute the same block differently and fall out of step. `NetworkConfig`
(in `protocol/network_config.x`) is the set of them, and this describes how a
block gets the one it runs under.

The parameters themselves are documented where they are declared. What is not
visible from any one file is the path they travel.

`GenesisConfiguration::network_config` sets what block 0 runs under. Whoever
creates a chain chooses its explicitly stated parameters:
`GenesisConfigurationBuilder::build` takes the configuration as an argument,
and `stock_network_config()` holds the values this repository's own dev and
test chains use.

Fresh `hog-devnode` chains (including those started by `thruput node`) use
256 KiB for RV32 code and 512 KiB for the complete contract artifact, including
its header and initial memory, without network-config overrides. These are
also the protocol defaults for omitted parameters. Configurations that state
explicit values retain them.

From there it rides the `DbTip`, exactly as `default_wallet` does:

- `dbsched_load_genesis_configuration` installs it via
  `RPCContext::set_network_config`; the genesis commit's `write_dbtip`
  persists it.
- `RPCContext::update_dbtip` mutates the tip in place, so every later commit
  carries the field forward.
- The scheduler copies it from the committed parent tip when it builds the
  nominated or validated tip (`rpc_from_coord.cc`).
- Each transaction pins the tip on its `SystemCallHandler` at its first
  admission check (`SystemCallHandler::init_db_tip`), so the executor reads
  individual parameters straight off that pin rather than through the shared
  `StorageSnapshotCache` on every frame.

`ConfigHashPreimage` pairs it with the storage cost model and the default
wallet, and `ProposalHeader::config_hash` commits to their hash -- so a block
records the rules it was executed under, and a validator holding a different
configuration rejects the proposal instead of diverging silently.

That hash is also what authenticates all three on a node's own disk. `<DBTIP`
and `<COSTMODEL` are global metadata, which `is_state_item` leaves out of
`HogDbDAO::compute_full_db_hash`, so a recovered tip agreeing with its own
`storage_long_hash` has said nothing about the configuration it carries.
`validate_recovered_tip` re-derives the config hash from the recovered tip at
startup and checks it against the one that tip's committed header states,
before hog_db serves anything (#3926). Past height 0 it also refuses a default
wallet with no contract row, which otherwise stays invisible until the first
root call to an undeployed address returns an unrecoverable execution error;
height 0 is exempt because a chain may name a wallet artifact its own genesis
ceremony deploys in the height-1 block.

Nothing changes the configuration after genesis yet. Because it is already
per-tip state rather than a compiled-in constant, adding that does not require
touching the sites that enforce it.
