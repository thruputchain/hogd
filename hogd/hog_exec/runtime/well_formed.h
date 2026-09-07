#pragma once

#include "contract.hh"
#include "transaction.hh"

namespace hogd {

// Structural checks a transaction must pass before the executor will run it.
// These read only the transaction -- no ledger state, no signatures -- so the
// mempool can run the same predicate on submission and drop what the executor
// would refuse anyway.
class HogexecAdmission {
public:
  // False for one attachment the executor will not accept. Currently: a
  // native contract, as the banned-redeploy check refuses every native id, so
  // an attached one could never be deployed by the transaction carrying it.
  static bool is_contract_attachable(const Contract &contract);

  // The whole-transaction form, for callers holding a transaction rather than
  // iterating its attachments. A false answer is
  // ExecResultMalformedTransaction in proposal mode and
  // ExecResultTxValidationFailure in validation mode: the same defect is ours
  // to drop when we are the proposer, and the proposer's fault when we are
  // not.
  static bool is_tx_well_formed(const Transaction &tx);
};

} // namespace hogd
