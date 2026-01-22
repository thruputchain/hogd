#pragma once

#include "indexer.hh"
#include "x23/async.h"

namespace hogd {

class IndexerBlockExplorerRpc {
public:
  using rpc_service = IndexerBlockExplorer1;

  static x23::Async<TxInfoResult>
  get_transaction_info(SignedTransactionHash tx_hash)
  {
    co_return TxInfoResult(TxInfoOutcome::TxInfoNotFound);
  }
};
static_assert(is_IndexerBlockExplorer1<IndexerBlockExplorerRpc *, x23::Async>);

} // namespace hogd
