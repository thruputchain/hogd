#pragma once

#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "transaction.hh"
#include "view.hh"
#include "x23/async.h"

namespace hogd {

// The most auth-token nodes, summed across every forwarded token, a single
// view query may carry. A view is local RPC, not consensus, and a public one
// is unauthenticated, so this is a DoS guard rather than a consensus rule:
// forwarded_tokens is an unbounded wire array, and the view admits each token
// (depth walk + registry mint) off the gas meter, so without a cap a caller
// could force work proportional to the whole RPC message. Depth is bounded per
// path but the aggregate node count is not, hence this. Generous for honest
// simulation (a full-depth token is 85 nodes), negligible as attack work.
inline constexpr uint32_t kMaxViewForwardedAuthTokenNodes = 4096;

class View {
public:
  View() = delete;

  // The syscalls the view runs read tip-derived values (executing height,
  // network hash, configuration parameters) off `snapshot_cache`, whose tip
  // the caller must have loaded or seeded before calling this.
  static x23::Async<ExpectedViewResult> run_view_transaction(
      StorageSnapshotCache &snapshot_cache, PerTransactionDatabaseProxy proxy,
      PerTransactionEventServerProxy event_proxy,
      TransactionViewQuery const &query, RuntimeContext &context,
      NondeterministicProposalData ndet);
};

} // namespace hogd
