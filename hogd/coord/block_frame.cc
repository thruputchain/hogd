#include "hogd/coord/block_frame.h"

#include <atomic>
#include <cassert>
#include <stdexcept>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

bool
BlockProposalFrame::try_add_transaction(SignedTransaction const &signed_tx)
{
  auto tx_hash = sha256(signed_tx.tx);

  auto map = transactions_.take();
  if (map->find(tx_hash) == map->end()) {
    map->emplace(tx_hash, ProposedTransaction{.signed_tx = signed_tx});
    return true;
  }
  return false;
}

void
BlockProposalFrame::cancel_add_transaction(SHA256Hash const &tx_hash)
{
  auto map = transactions_.take();
  [[maybe_unused]] auto erase_count = map->erase(tx_hash);
  assert(erase_count == 1);
}

void
BlockProposalFrame::finish_add_transaction(
    SHA256Hash const &tx_hash, NondeterministicProposal const &proposal)
{
  auto map = transactions_.take();
  auto it = map->find(tx_hash);
  assert(it != map->end());
  it->second.proposal_data = proposal;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
SHA256Hash
BlockProposalFrame::canonical_block_hash() const
{
  throw std::runtime_error("unimplemented");
}
// NOLINTEND(readability-convert-member-functions-to-static)

std::vector<ProposedTransaction>
BlockProposalFrame::to_list()
{
  auto map = transactions_.take();
  std::vector<ProposedTransaction> out;

  for (auto const &[_, tx] : *map) {
    out.emplace_back(tx);
  }
  return out;
}

const ProposedTransaction *
BlockValidationFrame::get_next_validation_transaction()
{
  uint32_t next_idx = validation_index_.fetch_add(1, std::memory_order_relaxed);
  return next_idx >= transactions_.size() ? nullptr : &transactions_[next_idx];
}

} // namespace hogd
