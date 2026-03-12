#include "hogd/hogdb/item_lock/deletion_lock.h"

#include <cassert>
#include <cstddef>
#include <span>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/prepared.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
DeletionLock::prepare_committed_changes(const LockContext &ctx,
                                        Prepared &prepared)
{
  if (ctx.key.is_address_auth_data()) {
    auto key =
        make_address_key(ctx.key.get_address(), Prefix::kAddressAuthData);
    prepared.remove(std::as_bytes(std::span(key)));
    return {};
  }

  if (!ctx.key.is_contract_data()) {
    error("unhandled item key type in DeletionLock: {}",
          std::to_underlying(ctx.key.key_type()));
    return HogDBError(HogDBCode::kInternal);
  }

  assert(ctx.dao);
  auto type_ret = ctx.dao->get_item_type(ctx.key);
  if (!type_ret) {
    if (type_ret.error() == HogDBCode::kNotFound) {
      return {};
    }
    return type_ret.error();
  }

  if (type_ret.value() == kRetrocausalQueue) {
    // Remove all RCQ blocks + metadata.
    auto meta_ret = ctx.dao->read_retrocausal_queue_metadata(ctx.key);
    if (meta_ret.has_value()) {
      auto const &meta = meta_ret.value();
      uint64_t start_block =
          meta.consume_state.max_fully_consumed_block_number + 1;
      for (uint64_t b = start_block; b <= meta.max_live_block_num; ++b) {
        auto block_key = make_retrocausal_queue_block_key(ctx.key, b);
        prepared.remove(std::as_bytes(std::span(block_key)));
      }
    }
    else if (meta_ret.error() != HogDBCode::kNotFound) {
      return meta_ret.error();
    }
    auto meta_key = make_contract_data_key(ctx.key.as_bytes());
    prepared.remove(meta_key);
  }
  else {
    auto key = make_contract_data_key(ctx.key.as_bytes());
    prepared.remove(key);
  }

  return {};
}

} // namespace hogd
