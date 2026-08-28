#pragma once

#include <string_view>
#include <utility>

#include "hogd/base/log.h"
#include "hogd/hog_indexer/db.h"
#include "hogd/hog_indexer/recent_tx_failure_cache.h"
#include "hogd/hog_indexer/status.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "network.hh"
#include "transaction.hh"
#include "x23/async.h"

namespace hogd {

class IndexerBlockExplorerRpc {
public:
  using rpc_service = IndexerBlockExplorer1;

  IndexerBlockExplorerRpc(IndexDB &db, RecentTxFailureCache &failure_cache)
    : db_(db), failure_cache_(failure_cache)
  {}

  static x23::Async<IndexerBlockExplorerIntroOutcome>
  indexer_block_explorer_intro(ProtocolCommit protocol)
  {
    if (std::string_view(protocol) !=
        std::string_view(version::kHogdProtocol())) {
      co_return IndexerBlockExplorerIntroOutcome::IntroMismatchedProtocol;
    }
    co_return IndexerBlockExplorerIntroOutcome::IntroSuccess;
  }

  x23::Async<TxInfoResult> get_transaction_info(const TransactionHash &tx_hash)
  {
    // Careful! If we change this function to use [co_await], but sure to copy
    // the [tx_hash] parameter above rather than keep it as a reference.
    const IndexerStatus<IndexDB::TxRecord> status = db_.get_tx_record(tx_hash);
    if (!status) {
      // No committed record: fall back to the short-lived failure cache so a
      // client can learn why a recently rejected tx never landed. Committed
      // records always win (checked first) -- a conflicted-then-retried tx
      // reports TxInfoFound.
      if (auto diag = failure_cache_.lookup(tx_hash)) {
        TxInfoResult ret(TxInfoOutcome::TxInfoRecentFailure);
        ret.recent_failure() = *diag;
        co_return ret;
      }
      co_return TxInfoResult(TxInfoOutcome::TxInfoNotFound);
    }
    const TxInfo info{.height = status->height,
                      .tx = status->tx,
                      .events = status->events};
    TxInfoResult ret(TxInfoOutcome::TxInfoFound);
    ret.info() = info;
    co_return ret;
  }

  static x23::Async<BlockLookupResult>
  get_block_by_height(BlockHeight /*height*/)
  {
    // The indexer is not a block store. Keep the protocol RPC conformant
    // without persisting a second copy of each ConsensusValue.
    co_return BlockLookupResult(BlockLookupOutcome::BlockNotFound);
  }

  x23::Async<EventPageResult> get_events(EventQuery query)
  {
    auto status = db_.get_events(query);
    if (!status) {
      co_return query_failure<EventPageResult>("get_events", status.error());
    }
    EventPageResult ret(IndexerQueryOutcome::IndexerQueryOk);
    ret.page() = std::move(*status);
    co_return ret;
  }

  x23::Async<TransactionPageResult> get_transactions(TransactionQuery query)
  {
    auto status = db_.get_transactions(query);
    if (!status) {
      co_return query_failure<TransactionPageResult>("get_transactions",
                                                     status.error());
    }
    TransactionPageResult ret(IndexerQueryOutcome::IndexerQueryOk);
    ret.page() = std::move(*status);
    co_return ret;
  }

  x23::Async<TransactionRefPageResult>
  get_transactions_by_address(AddressTransactionQuery query)
  {
    auto status = db_.get_transactions_by_address(query);
    if (!status) {
      co_return query_failure<TransactionRefPageResult>(
          "get_transactions_by_address", status.error());
    }
    TransactionRefPageResult ret(IndexerQueryOutcome::IndexerQueryOk);
    ret.page() = std::move(*status);
    co_return ret;
  }

  x23::Async<TransactionRefPageResult>
  get_transactions_by_block(BlockTransactionQuery /*query*/)
  {
    // The transaction index is keyed by transaction hash and has no
    // block-height index yet.  Report the current serving bounds while
    // returning no refs until that index is added.
    auto watermark = db_.latest_block();
    if (!watermark) {
      co_return query_failure<TransactionRefPageResult>(
          "get_transactions_by_block", watermark.error());
    }
    TransactionRefPageResult ret(IndexerQueryOutcome::IndexerQueryOk);
    ret.page().latest_block = *watermark;
    ret.page().oldest_block = IndexDB::oldest_block();
    co_return ret;
  }

private:
  // query_failure maps a db-level error onto a page-query result union so a
  // broken or not-yet-open index is distinguishable from an empty result set:
  // kDBNotOpen is reported as IndexerQueryNotReady, everything else (corrupt
  // key, failed XDR parse, RocksDB iterator error) as
  // IndexerQueryInternalFailure.
  template<typename Result>
  static Result query_failure(std::string_view rpc, const IndexerError &err)
  {
    error("indexer {} query failed: {}", rpc, err.error_code().message());
    const auto outcome = err.code() == IndexerErrorCode::kDBNotOpen
                             ? IndexerQueryOutcome::IndexerQueryNotReady
                             : IndexerQueryOutcome::IndexerQueryInternalFailure;
    return Result(outcome);
  }

  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
};
static_assert(is_IndexerBlockExplorer1<IndexerBlockExplorerRpc *, x23::Async>);

} // namespace hogd
