#pragma once

#include "hogd/indexer/db.h"
#include "indexer.hh"
#include "x23/async.h"

namespace hogd {

class IndexerBlockExplorerRpc {
public:
  using rpc_service = IndexerBlockExplorer1;

  IndexerBlockExplorerRpc(IndexDB &db) : db_(db) {}

  x23::Async<TxInfoResult>
  get_transaction_info(const SignedTransactionHash &tx_hash)
  {
    // Careful! If we change this function to use [co_await], but sure to copy
    // the [tx_hash] parameter above rather than keep it as a reference.
    const IndexerStatus<std::pair<BlockHeight, TxEventLog>> status =
        db_.get_tx_events(tx_hash);
    if (!status) {
      co_return TxInfoResult(TxInfoOutcome::TxInfoNotFound);
    }
    const TxInfo info{.height = status->first, .events = status->second};
    TxInfoResult ret(TxInfoOutcome::TxInfoFound);
    ret.info() = info;
    co_return ret;
  }

private:
  IndexDB &db_;
};
static_assert(is_IndexerBlockExplorer1<IndexerBlockExplorerRpc *, x23::Async>);

} // namespace hogd
