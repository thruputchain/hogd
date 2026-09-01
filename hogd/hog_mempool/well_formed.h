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
// It is a filter, not the authority, and it applies to *unvetted*
// submissions only -- the ones a client sends us, which we might put in a
// block we propose. What it buys is that a transaction we would certainly
// reject later does not occupy a mempool slot, get authenticated, or reach a
// block we propose.
//
// Do not apply it to vetted submissions. Those arrive from the coordinator so
// this node can validate a block a *peer* proposed, and dropping one makes
// that block permanently unvalidatable here -- the same wedge
// schedulercoord_submit_vetted() skips its capacity check to avoid. hog_exec
// judges that case correctly on its own: a native attachment reaching the
// executor in validation mode is a proposer-attributable ValidationFailure.
// Filtering what we propose is ours; judging what a peer proposed is not.
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
