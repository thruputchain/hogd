#include "hogd/hogdb/item_lock/deletion_lock.h"

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include <utility>

namespace hogd {

HogDBStatus<void>
DeletionLock::write_committed_change(LockContext const &ctx,
                                     MerkleTriePrefix &trie)
{
  int8_t type = 0;
  if (ctx.key.is_account_auth_data()) {
    type = 0;
  }
  else if (ctx.key.is_contract_data()) {
    auto type_ret = ctx.dao.get_item_type(ctx.key);
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

  HogDBStatus<void> remove_ret = ctx.dao.remove_item(ctx.key);
  if (!remove_ret) {
    return remove_ret;
  }
  // [MerkleTrieHelpers::remove_kv_pair_from_trie] will always succeed as we
  // will never attempt to remove a key that does not exist. We already checked
  // above that the key exists when we removed it from the database.
  CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, ctx.key, type)
            .has_value());
  return {};
}

} // namespace hogd
