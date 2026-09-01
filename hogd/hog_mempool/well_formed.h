#pragma once

#include "transaction.hh"

namespace hogd {

// Structural checks the mempool makes before admitting a transaction. These
// read only the transaction -- no ledger state, no signatures -- so they are
// cheap enough to run on every submission.
class MempoolAdmission {
public:
  // Unvetted submissions only: dropping a vetted tx leaves the peer block it
  // belongs to permanently unvalidatable here, and hog_exec rejects these
  // correctly on its own.
  //
  // Currently: no attached contract is native, as the executor's
  // banned-redeploy check refuses every native id.
  static bool is_tx_well_formed(const SignedTransaction &tx);
};

} // namespace hogd
