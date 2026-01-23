#pragma once

#include "hogd/indexer/db.h"
#include "indexer.hh"
#include "x23/async.h"

namespace hogd {

class IndexerBlockExplorerRpc {
public:
  using rpc_service = IndexerBlockExplorer1;

  IndexerBlockExplorerRpc(IndexDB &db) : db_(db) {}

  static x23::Async<TxInfoResult>
  get_transaction_info(SignedTransactionHash tx_hash)
  {
    co_return TxInfoResult(TxInfoOutcome::TxInfoNotFound);
  }

private:
  IndexDB &db_;
};
static_assert(is_IndexerBlockExplorer1<IndexerBlockExplorerRpc *, x23::Async>);

} // namespace hogd
