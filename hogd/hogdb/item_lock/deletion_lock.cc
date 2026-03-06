#include "hogd/hogdb/item_lock/deletion_lock.h"

#include <cassert>
#include <cstddef>
#include <span>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/db_hash.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/item_lock/retrocausal_queue_lock.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
DeletionLock::write_committed_change(LockContext const &ctx)
{
  int8_t type = 0;
  if (ctx.key.is_address_auth_data()) {
    type = 0;
  }
  else if (ctx.key.is_contract_data()) {
    assert(ctx.dao);
    auto type_ret = ctx.dao->get_item_type(ctx.key);
    if (!type_ret) {
      // Deleting a non-existent key is a no-op.
      if (type_ret.error() == HogDBCode::kNotFound) {
        return {};
      }
      return type_ret.error();
    }
    type = type_ret.value();
  }
  else {
    error("unhandled item key type in DeletionLock: {}",
          std::to_underlying(ctx.key.key_type()));
    return HogDBError(HogDBCode::kInternal);
  }

  assert(ctx.dao);

  if (type == kRetrocausalQueue) {
    // Delegate to RCQ-specific cleanup: removes metadata + all blocks from DAO.
    // This is fully self-contained.
    auto cleanup_ret = RetrocausalQueueLock::cleanup_on_deletion(ctx);
    if (!cleanup_ret.has_value()) {
      return cleanup_ret.error();
    }
  }
  else {
    // For non-RCQ types, use the standard remove_item.
    HogDBStatus<void> remove_ret = ctx.dao->remove_item(ctx.key);
    if (!remove_ret) {
      return remove_ret;
    }
  }

  return {};
}

HogDBStatus<void>
DeletionLock::apply_db_hash_delta(const LockContext &ctx, LtHash16 &hash)
{
  assert(ctx.dao);

  auto remove_if_present = [&](std::span<const std::byte> key) {
    auto before = ctx.dao->get_ramdb().get(key);
    if (before.has_value()) {
      lthash_remove_item(hash, key, before->type, before->value);
    }
  };

  if (ctx.key.is_address_auth_data()) {
    auto key = make_address_key_with_prefix(ctx.key.get_address(),
                                            Prefix::kAddressAuthData);
    remove_if_present(std::as_bytes(std::span(key)));
    return {};
  }

  if (!ctx.key.is_contract_data()) {
    error("unhandled item key type in DeletionLock::apply_db_hash_delta: {}",
          std::to_underlying(ctx.key.key_type()));
    return HogDBError(HogDBCode::kInternal);
  }

  auto key = make_contract_data_db_key(ctx.key.as_bytes());

  auto type_ret = ctx.dao->get_item_type(ctx.key);
  if (!type_ret) {
    if (type_ret.error() == HogDBCode::kNotFound) {
      return {};
    }
    return type_ret.error();
  }

  if (type_ret.value() == kRetrocausalQueue) {
    auto meta_ret = ctx.dao->read_retrocausal_queue_metadata(ctx.key);
    if (meta_ret.has_value()) {
      auto const &meta = meta_ret.value();
      uint64_t start_block =
          meta.consume_state.max_fully_consumed_block_number + 1;
      for (uint64_t b = start_block; b <= meta.max_live_block_num; ++b) {
        auto block_key = make_retrocausal_queue_block_key(ctx.key, b);
        remove_if_present(std::as_bytes(std::span(block_key)));
      }
    }
    else if (meta_ret.error() != HogDBCode::kNotFound) {
      return meta_ret.error();
    }
  }

  remove_if_present(key);
  return {};
}

} // namespace hogd
