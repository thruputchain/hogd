// clang-format off
%#include "rpc_types.hh"
%#include "transaction.hh"
namespace hogd {
  // clang-format on

  enum ExecResult {
    ExecResultOK = 0,
    // Transaction execution (during proposal or validation) failed;
    // the error is deterministic and can be replayed in validation.
    // Transactions with this error code can be included in the block.
    ExecResultTxDeterministicError = 1,
    // Transaction execution (during proposal or validation) failed,
    // due to nondeterminism during the post-execution 2-phase commit.
    // Transactions with this error code should not be included in a proposal,
    // and indicate a block validation failure.
    ExecResultTxNonDeterministicError = 2,
    // Validation on the transaction has failed because the attached validation
    // data is bad. E.g. missing data or unused data. This error code is only
    // returned during validation. This is determininistic, but signals a bad
    // block from a malicious proposer.
    ExecResultTxValidationFailure = 3,
    // System error; network connection broke, etc.
    // Executor can recover from this error to continue execution of other
    // transactions.
    ExecResultSystemError = 4,
    // Critical failure in the executor, which cannot be recovered from.
    // E.g. OOM or critical logic error.
    ExecResultUnrecoverable = 5,
  };

  union ExecProposeResult switch (ExecResult result)
  {
  case ExecResultOK:
  case ExecResultTxDeterministicError:
    NondeterministicProposalData proposal_data;
  default:
    void;
  };

  typedef SignedTransaction SignedTransactionBatch<>;
  typedef ProposedTransaction ProposedTransactionBatch<>;
  typedef ExecProposeResult ExecProposeBatchResult<>;
  typedef ExecResult ExecValidateBatchResult<>;

  program ExecutorCoord
  {
    version ExecutorCoord1
    {

      void executorcoord_null(void) = 0;

      ExecProposeResult executorcoord_propose_transaction(BlockID,
                                                          SignedTransaction) =
          10;
      ExecResult executorcoord_validate_transaction(BlockID,
                                                    ProposedTransaction) = 20;

      ExecProposeBatchResult executorcoord_propose_batch(
          BlockID, SignedTransactionBatch) = 11;
      ExecValidateBatchResult executorcoord_validate_batch(
          BlockID, ProposedTransactionBatch) = 21;

      // Executor can be in one of three states: None, Proposing, Validating
      // open_xyz_block() methods set the executor state, or return an error
      // if executor has already opened a block.
      // finish_block() blocks until all running executor threads are finished,
      // and stops the executor from accepting new transactions.
      ExecResult executorcoord_open_proposal_block(BlockID) = 30;
      ExecResult executorcoord_open_validation_block(BlockID) = 40;
      void executorcoord_finish_block(BlockID) = 50;

      // Coord should call this to shutdown the executor.
      // Shutdown will not occur until all pending transactions complete.
      // No new transactions or blocks can be opened after shutdown.
      void executorcoord_shutdown(void) = 60;
    }
    = 1;
  }
  = 0x7ee79999;

} // namespace hogd
