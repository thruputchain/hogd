#include "hogd/db_interface/db_sched_server.h"

#include "consensus.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/storage_cost/storage_cost_model.h"
#include "rpc_types.hh"
#include "x23/async.h"

namespace hogd {

x23::Async<DbSchedResult>
DbSchedRpc::dbsched_create_snapshot(BlockID parent_id, BlockID new_id,
                                    BlockExecutionMode mode)
{
  if (block_manager_->add_child(parent_id, new_id, mode)) {
    co_return DbSchedResult::DbSchedResultOK;
  }
  co_return DbSchedResult::DbSchedResultError;
}

x23::Async<DbBlockResult>
DbSchedRpc::dbsched_commit_snapshot(BlockID id, ConsensusValue value)
{
  if (block_manager_->commit_block(id)) {
    DbBlockResult result(DbSchedResult::DbSchedResultOK);
    result.block().config_hash_preimage =
        make_config_hash_preimage(*block_manager_->get_cost_model());
    co_return result;
  }
  DbBlockResult result(DbSchedResult::DbSchedResultError);
  co_return result;
}

x23::Async<DbBlockResult>
DbSchedRpc::dbsched_close_snapshot(BlockID id)
{
  auto blk = block_manager_->get_block_data(id);
  if (!blk) {
    DbBlockResult result(DbSchedResult::DbSchedResultError);
    co_return result;
  }
  blk->close_block();
  DbBlockResult result(DbSchedResult::DbSchedResultOK);
  result.block().config_hash_preimage =
      make_config_hash_preimage(*block_manager_->get_cost_model());
  // TODO(geoff): Set block_hash if available
  // result.block_hash = ...; // Set block_hash if available
  co_return result;
}

x23::Async<DbSchedResult>
DbSchedRpc::dbsched_abandon_snapshot(BlockID id)
{
  // TODO(geoff): Implement actual abandon logic if needed
  // For now, just return ok for stub. If and when this function can return a
  // status other than [DbSchedResultOK], the mempool will need to be updated
  // (please coordinate with jhumphri@ on this).
  co_return DbSchedResult::DbSchedResultOK;
}

x23::Async<DbSchedResult>
DbSchedRpc::dbsched_load_genesis_configuration(
    GenesisConfiguration genesis_configuration, ConsensusValue value)
{
  if (value.proposal_header.block_height != 0 ||
      value.proposal_header.network_hash !=
          genesis_configuration.network_hash ||
      value.proposal_header.config_hash !=
          compute_config_hash(genesis_configuration.storage_cost_model)) {
    co_return DbSchedResult::DbSchedResultError;
  }
  if (block_manager_->initialize_genesis_configuration(genesis_configuration)) {
    co_return DbSchedResult::DbSchedResultOK;
  }
  co_return DbSchedResult::DbSchedResultError;
}

} // namespace hogd
