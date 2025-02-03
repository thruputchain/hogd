#include "hogd/coord/coord_core.h"

#include "hogd/coord/coord_block_manager.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace hogd {

CoordinatorCore::CoordinatorCore(int nthreads)
  : pool_(nthreads), cp_(pool_), allocate_service_(cp_, executor_manager_)
{}

void
CoordinatorCore::start(std::string const &coord_server_port)
{
  allocate_service_.start(coord_server_port);
  db_manager_ = allocate_service_.make_database_manager();
  block_manager_ = std::make_unique<CoordinatorBlockManager>(*db_manager_);
}

x23::Async<bool>
CoordinatorCore::validate_block(SHA256Hash const &parent_hash,
                                std::vector<ProposedTransaction> txs,
                                SHA256Hash const &block_hash)
{
  auto parent_id = block_manager_->lookup_block_id(parent_hash);
  if (!parent_id) {
    co_return false;
  }

  auto block_id =
      block_manager_->add_block_to_validate(*parent_id, txs, block_hash);
  if (!block_id) {
    co_return false;
  }

  auto block = block_manager_->get_block(*block_id);
  // possible that the block gets pruned immediately before we start validation
  if (!block) {
    co_return false;
  }

  auto *frame = block->get_validation_frame();

  co_await executor_manager_.enqueue_validation_request(*block_id, frame);

  if (frame->validation_success()) {
    auto finish_result = co_await block_manager_->close_block(*block_id);
    if (!finish_result.has_value()) {
      block->mark_validation_failed();
      block_manager_->prune_block(*block_id);
      co_return false;
    }

    block->mark_validation_success();
    assert(*finish_result == block_hash);
    co_return true;
  }
  else {
    block->mark_validation_failed();
    block_manager_->prune_block(*block_id);
    co_return false;
  }
  // otherwise, return false

  // frame is still a valid pointer because we hold a shared_ptr to the block
  // (the block variable).
  co_return frame->validation_success();
}

void
CoordinatorCore::commit_block(SHA256Hash const &block_hash)
{
  auto block_id = block_manager_->lookup_block_id(block_hash);
  assert(block_id);
  block_manager_->get_block(*block_id)->mark_as_committed();
}

} // namespace hogd
