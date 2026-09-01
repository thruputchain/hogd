#pragma once

#include "transaction.hh"

namespace hogd {

// Structural conditions a transaction must satisfy for the mempool to admit
// it. This is deliberately self-contained: it inspects only the transaction
// itself, with no ledger state, block height, or signature check, so it is
// cheap enough to run on every submission before the auth round-trip and
// gives one place to point at for "what does the mempool consider a
// well-formed transaction".
//
// It is a filter, not the authority. The executor re-checks everything that
// matters for consensus, because a transaction can also arrive in a block
// proposed by another node, which never passes through this mempool. What
// this buys is that a transaction we would certainly reject later does not
// occupy a mempool slot, get authenticated, or reach a block we propose.
//
// Conditions:
//
//   - No attached contract is native. Native contracts are registered at
//     genesis and every id is refused by the executor's banned-redeploy
//     check, so an attached one can never be deployed by the transaction
//     carrying it. hog_exec treats one that reaches it as Unrecoverable in
//     proposal mode precisely because this filter should have dropped it.
//
// Candidates for later: the attached-blob size cap (kMaxContractSizeBytes),
// which hog_exec enforces in perform_initial_validation and which would need
// its contract_blob_size helper lifted somewhere shared first.
bool is_tx_well_formed(SignedTransaction const &tx);

} // namespace hogd
