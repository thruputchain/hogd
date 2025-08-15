// clang-format off
%#include "discovery.hh"
%#include "nomination.hh"
%#include "transaction.hh"
%#include "types.hh"

namespace hogd {
  // clang-format on

  struct CatchupBlock {
    BlockHeight block_height;
    BlockHash block_hash;
    BlockHash prev_block_hash;
    ProposedTransaction proposed_transactions<>;
  };

  struct CatchupSpan {
    BlockHeight from_block_height;
    BlockHeight to_block_height; // not including
  };

  struct CatchupBlocksRequest {
    Timestamp timestamp;
    CatchupSpan spans<>;
  };

  struct SignedCatchupBlocksRequest {
    ValidatorPublicInfo signer;
    Signature signature;
    CatchupBlocksRequest request;
  };

  struct CatchupBlocksResult {
    Timestamp timestamp;
    CatchupBlock blocks<>;
  };

  struct SignedCatchupBlocksResult {
    ValidatorPublicInfo signer;
    Signature signature;
    CatchupBlocksResult result;
  };

  // Catchup service enables validators that are lagging behind SCP to recover a
  // small number of missed blocks.
  program Catchup
  {
    version Catchup1
    {

      void catchup_null(void) = 0;

      SignedCatchupBlocksResult catchup_solicit(SignedCatchupBlocksRequest) =
          10;
    }
    = 1;
  }
  = 0x4321abcd;

} // namespace hogd
