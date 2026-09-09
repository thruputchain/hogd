#pragma once

#include "hogd/hog_exec/runtime/event_server_proxy.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "transaction.hh"
#include "view.hh"
#include "x23/async.h"

namespace hogd {

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
