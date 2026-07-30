#pragma once

#include <string_view>
#include <utility>

#include "hogd/indexer/db.h"
#include "hogd/indexer/recent_tx_failure_cache.h"
#include "hogd/indexer/status.h"
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

  x23::Async<TxInfoResult>
  get_transaction_info(const SignedTransactionHash &tx_hash)
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

  x23::Async<EventPage> get_events(EventQuery query)
  {
    auto status = db_.get_events(query);
    if (!status) {
      // Return an empty page on db-level error.  Currently the only
      // way to hit this is kDBNotOpen; higher layers surface that
      // out-of-band.
      co_return EventPage{};
    }
    co_return std::move(*status);
  }

  x23::Async<TransactionPage> get_transactions(TransactionQuery query)
  {
    auto status = db_.get_transactions(query);
    if (!status) {
      co_return TransactionPage{};
    }
    co_return std::move(*status);
  }

  x23::Async<TransactionRefPage>
  get_transactions_by_address(AddressTransactionQuery query)
  {
    auto status = db_.get_transactions_by_address(query);
    if (!status) {
      co_return TransactionRefPage{};
    }
    co_return std::move(*status);
  }

  x23::Async<TransactionRefPage>
  get_transactions_by_block(BlockTransactionQuery /*query*/)
  {
    // The transaction index is keyed by transaction hash and has no
    // block-height index yet.  Report the current serving bounds while
    // returning no refs until that index is added.
    TransactionRefPage page;
    page.latest_block = db_.latest_block();
    page.oldest_block = IndexDB::oldest_block();
    co_return page;
  }

private:
  IndexDB &db_;
  RecentTxFailureCache &failure_cache_;
};
static_assert(is_IndexerBlockExplorer1<IndexerBlockExplorerRpc *, x23::Async>);

} // namespace hogd
