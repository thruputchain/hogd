#include "hogd/hogdb/item_lock/new_contract_lock.h"

#include <cassert>
#include <span>

#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/item_change.h"
#include "hogd/hogdb/item_lock/concepts.h"
#include "hogd/hogdb/prepared.h"
#include "hogd/hogdb/status.h"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

HogDBStatus<int128_t>
NewContractLock::try_atomic_change_lock(const NewContractData *before,
                                        const NewContractData *after,
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
NewContractLock::prepare_committed_changes(const LockContext &ctx,
                                           Prepared &prepared) const
{
  SHA256Hash code_hash = ctx.key.get_contract_code_hash();
  auto key = make_wasm_code_key(code_hash);
  auto buf = x23::serialize(contract_);
  prepared.put(std::as_bytes(std::span(key)), 0, std::as_bytes(std::span(buf)));
  return {};
}

} // namespace hogd
