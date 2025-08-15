// clang-format off
%#include "transaction.hh"
%#include "types.hh"

namespace hogd {
  // clang-format on

  // BlockHeight is the height of a block. It is signed to prevent overflows in
  // calculations.
  typedef hyper BlockHeight;

  typedef SHA256Hash BlockHash;

  struct Nomination {
    BlockHeight block_height;
    BlockHash block_hash;
    BlockHash prev_block_hash;
    SignedTransactionHash transaction_hashes<>;
    NominatedTransaction transaction_nominations<>;
  };

  struct PrevNominationSummary {
    BlockHash block_hash;
  };

} // namespace hogd
