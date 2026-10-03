#pragma once

#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "transaction.hh"
#include "view.hh"
#include "x23/async.h"

namespace hogd {

// Cap on the total auth-token nodes across a view query's forwarded tokens: a
// DoS guard for an unauthenticated admission path that runs off the gas meter
// (forwarded_tokens is unbounded), not a consensus rule. See the PR/#4314 for
// the rationale.
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
