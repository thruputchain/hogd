#include "hogd/hogdb/item_lock/deletion_lock.h"

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"

namespace hogd {

HogDBStatus<void>
DeletionLock::write_committed_change(ItemKey const &key, HogDBDAO &dao,
                                     MerkleTriePrefix &trie)
{

  HogDBStatus<int8_t> type_ret = dao.get_item_type(key);
  if (!type_ret) {
    // Deleting a non-existent key is a no-op.
    if (type_ret.error() == HogDBCode::kNotFound) {
      return {};
    }
    return type_ret.error();
  }

  HogDBStatus<void> remove_ret = dao.remove_item(key);
  if (!remove_ret) {
    return remove_ret;
  }
  // [MerkleTrieHelpers::remove_kv_pair_from_trie] will always succeed as we
  // will never attempt to remove a key that does not exist. We already checked
  // above that the key exists when we removed it from the database.
  CHECK(MerkleTrieHelpers::remove_kv_pair_from_trie(trie, key, type_ret.value())
            .has_value());
  return {};
}

} // namespace hogd
