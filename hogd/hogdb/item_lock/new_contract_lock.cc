#include "hogd/hogdb/item_lock/new_contract_lock.h"

#include <cassert>
#include <span>

#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "x23/serialize.h"

namespace hogd {

HogDBStatus<int128_t>
NewContractLock::try_atomic_change_lock(const NewContractData *before,
                                        const NewContractData *after,
                                        const StorageCostModel * /*unused*/,
                                        const LockContext & /*ctx*/)
{
  assert(before == nullptr || before->new_contract() == contract_);

  if (after != nullptr && after->new_contract() != contract_) {
    return HogDBError{HogDBCode::kConflict};
  }

  // pay on contract deployment, not contract create
  return 0;
}

HogDBStatus<void>
NewContractLock::write_committed_change(LockContext const &ctx,
                                        MerkleTriePrefix & /*trie*/) const
{
  auto buf = x23::serialize(contract_);
  return ctx.dao.write_wasm_code(ctx.key.get_contract_code_hash(),
                                 std::as_bytes(std::span(buf)));
}

} // namespace hogd
