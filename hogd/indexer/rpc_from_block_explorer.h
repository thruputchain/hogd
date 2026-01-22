#pragma once

#include "indexer.hh"

namespace hogd {

class IndexerBlockExporerRpc {
public:
  using rpc_service = IndexerBlockExplorer1;

  static x23::Async<TxInfoResult>
  get_transaction_info(SignedTransactionHash tx_hash)
  {
    co_return TxInfoResult(TxInfoOutcome::TxInfoNotFound);
  }
};
static_assert(is_IndexerBlockExplorer1<IndexerBlockExporerRpc *, x23::Async>);

} // namespace hogd
