#pragma once

#include <unordered_map>
#include <unordered_set>

#include "contract.hh"
#include "hogd/hog_exec/components/borrowed_contract_batch.h"
#include "hogd/hog_exec/runtime/prepared_contract.h"
#include "types.hh"

namespace hogd {

class NewContractSet {
public:
  // An empty set: every lookup misses. SystemCallHandler holds one from
  // construction so lookups before (or without) initial validation are
  // well-defined.
  NewContractSet() = default;

  NewContractSet(NewContractSet &&) = default;
  NewContractSet &operator=(NewContractSet &&) = default;

  NewContractSet(NewContractSet const &) = delete;
  NewContractSet &operator=(NewContractSet const &) = delete;

  // Borrows [contracts]: the returned set holds pointers into its elements,
  // so they must outlive the set (buffer-stealing moves of the enclosing
  // vector are fine; destroying the elements is not).
  static std::pair<InvokeStatus<NewContractSet>, GasAmount>
  make(NewContractBatch const &contracts, RuntimeContext &runtime_context);

  BorrowedContractBatch borrow_contracts(
      std::unordered_set<SHA256Hash, SipHash24Hasher> const &hashes) const
  {
    return BorrowedContractBatch(hashes, new_contracts_);
  }

  const Contract *get_contract_code(SHA256Hash const &hash) const
  {
    auto it = new_contracts_.find(hash);
    if (it == new_contracts_.end()) {
      return nullptr;
    }
    return it->second;
  }

  const PreparedContract *get_prepared_contract(SHA256Hash const &hash) const
  {
    auto it = new_contract_preparations_.find(hash);
    if (it == new_contract_preparations_.end()) {
      return nullptr;
    }
    return &it->second;
  }

private:
  // TODO(geoff): these can be combined into one map
  std::unordered_map<SHA256Hash, const Contract *, SipHash24Hasher>
      new_contracts_;
  std::unordered_map<SHA256Hash, PreparedContract, SipHash24Hasher>
      new_contract_preparations_;
};

} // namespace hogd
