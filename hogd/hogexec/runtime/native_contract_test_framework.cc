#include "hogd/hogexec/runtime/native_contract_test_framework.h"

#include <cassert>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "contract.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"

namespace hogd {

void
NativeContractTestFramework::advance_block(
    BlockExecutionMode mode,
    DbSchedResult expected_dbsched_commit_snapshot_result)
{
  {
    auto r = coord_wrapper_->client().dbsched_close_snapshot(block_number_);
    if (r.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("advance_block: dbsched_close_snapshot failed");
    }
  }
  {
    auto r = coord_wrapper_->client().dbsched_commit_snapshot(
        block_number_,
        // NOLINTNEXTLINE(bugprone-argument-comment)
        /*block_hash=*/{}, /*events_hash=*/{});
    if (r.result() != expected_dbsched_commit_snapshot_result) {
      throw std::runtime_error(std::format(
          "advance_block: mismatching dbsched_commit_snapshot result: {} != {}",
          r.result(), expected_dbsched_commit_snapshot_result));
    }
  }
  close_event_block(block_number_);
  dbtip_block_id_ = block_number_;
  block_number_++;

  {
    auto r =
        coord_wrapper_->client().dbsched_create_snapshot(dbtip_block_id_,
                                                         block_number_, mode);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("advance_block: dbsched_create_snapshot failed");
    }
  }
  open_event_block(block_number_);

  storage_snapshot_cache_ =
      std::make_shared<StorageSnapshotCache>(block_number_, *db_proxy_);
}

void
NativeContractTestFramework::reject_block(BlockExecutionMode mode)
{
  // Capture the db hash before the block close attempt.
  auto tip_before = get_db_tip();

  // Close should fail — prepare_committed_changes detected an error
  // (e.g. consumed_set mismatch). No DAO mutations have occurred.
  {
    auto r = coord_wrapper_->client().dbsched_close_snapshot(block_number_);
    if (r.result() != DbSchedResult::DbSchedResultError) {
      throw std::runtime_error(
          "reject_block: expected dbsched_close_snapshot to fail");
    }
  }

  // Abandon the block — clean up without committing.
  {
    auto r = coord_wrapper_->client().dbsched_abandon_snapshot(block_number_);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("reject_block: dbsched_abandon_snapshot failed");
    }
  }

  // Verify DAO is unchanged: since prepare failed before any mutation,
  // the db hash must be identical to what it was before the block. Read the
  // dbtip directly from the DAO rather than through the executor RPC: after
  // abandon, the per-block executor view of the tip is Abandon. Reading
  // through the DAO means reject_block only works with HogdbWrapper.
  assert(std::holds_alternative<HogdbWrapper>(db_wrapper_));
  auto tip_after = std::get<HogdbWrapper>(db_wrapper_).get_db().read_dbtip();
  if (!tip_after) {
    throw std::runtime_error(
        "reject_block: failed to read dbtip from DAO after abandon");
  }
  if (tip_before.db_hash != tip_after->db_hash) {
    throw std::runtime_error(
        "reject_block: db hash changed after block rejection — "
        "DAO was mutated before prepare failure");
  }

  close_event_block(block_number_);
  // dbtip_block_id_ unchanged — block was not committed.
  block_number_++;

  {
    auto r =
        coord_wrapper_->client().dbsched_create_snapshot(dbtip_block_id_,
                                                         block_number_, mode);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("reject_block: dbsched_create_snapshot failed");
    }
  }
  open_event_block(block_number_);

  storage_snapshot_cache_ =
      std::make_shared<StorageSnapshotCache>(block_number_, *db_proxy_);
}

void
NativeContractTestFramework::discard_block(BlockExecutionMode mode)
{
  {
    auto r = coord_wrapper_->client().dbsched_close_snapshot(block_number_);
    if (r.result() != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("advance_block: dbsched_close_snapshot failed");
    }
  }
  {
    auto r = coord_wrapper_->client().dbsched_abandon_snapshot(block_number_);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error(
          "advance_block: dbsched_abandon_snapshot failed");
    }
  }

  close_event_block(block_number_);
  block_number_++;

  {
    auto r =
        coord_wrapper_->client().dbsched_create_snapshot(dbtip_block_id_,
                                                         block_number_, mode);
    if (r != DbSchedResult::DbSchedResultOK) {
      throw std::runtime_error("advance_block: dbsched_create_snapshot failed");
    }
  }
  open_event_block(block_number_);

  storage_snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
}

void
NativeContractTestFramework::enable_logging()
{
  previous_logger_ =
      swap_logger(std::make_unique<ConsoleLogger>(LogLevel::kTrace));
  x23::RpcLogger::default_logfd = 1;
}

void
NativeContractTestFramework::setup_grant_storage_token(
    ContractAddress const &address, int128_t amount)
{
  assert(amount >= 0);
  assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.grant_storage_token(address, amount);
      },
      db_wrapper_);
}

void
NativeContractTestFramework::write_item(
    AddressAndKey const &addrkey,
    std::shared_ptr<const StorageItemWrapper> item)
{
  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.write_item(addrkey, std::move(item));
      },
      db_wrapper_);
}
void
NativeContractTestFramework::write_contract(
    std::shared_ptr<const Contract> const &contract)
{
  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.write_contract(sha256(*contract), contract);
      },
      db_wrapper_);
}
void
NativeContractTestFramework::write_deployment(
    ContractAddress const &addr, DeployedContractHash const &deployment,
    StorageCostModel const *cost_model)
{
  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.write_deployment(addr, deployment, cost_model);
      },
      db_wrapper_);
}

void
NativeContractTestFramework::write_deployment(
    ContractAddress const &addr,
    std::shared_ptr<const Contract> const &contract)
{
  write_deployment(addr, make_deployed_contract_hash(contract),
                   get_default_cost_model());
}

void
NativeContractTestFramework::run_genesis_initialization(
    GenesisConfiguration const &genesis_configuration, BlockExecutionMode mode)
{
  assert(!storage_snapshot_cache_);
  assert(block_number_ == 0);

  uint64_t network_id = 0x0;
  SHA256Hash txset_hash{};
  SHA256Hash events_hash{};
  auto r = coord_wrapper_->client().dbsched_load_genesis_configuration(
      genesis_configuration, network_id, txset_hash, events_hash);
  if (r != DbSchedResult::DbSchedResultOK) {
    throw std::runtime_error(
        "run_genesis_initialization: dbsched_load_genesis_block failed");
  }
  open_block(mode);
}

void
NativeContractTestFramework::open_event_block(BlockID block_id)
{
  // rpc is infallible
  event_sched_wrapper_->client().eventsched_open_block(block_id);
}

void
NativeContractTestFramework::close_event_block(BlockID block_id)
{
  // We use [block_id] as the height as well.
  auto r =
      event_sched_wrapper_->client().eventsched_close_block(block_id, block_id);
  if (r.error() != RPCError::kRPCNone) {
    throw std::runtime_error(
        "close_event_block: eventsched_close_block failed");
  }
}

} // namespace hogd
