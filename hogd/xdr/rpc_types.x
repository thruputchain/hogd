// clang-format off

%#include "types.hh"
%#include "contract.hh"
%#include "transaction.hh"
%#include "storage_item.hh"

namespace hogd
{
  // clang-format on

  // An internal unique ID describing a block.
  typedef unsigned hyper BlockID;

  // This is obviously missing error codes.  Please expand or change as you
  // wish.
  // This probably should get moved to a different file?
  enum RPCError {
    kNone = 0,
    // Lookup on contract that does not exist in database
    kContractNExist = 1,
    // Lookup deployment of contract on address that has no deployed contract
    kNoContractDeployed = 2,
    // Executor in bad state to validate or propose
    kBadExecutorState = 3,
    // BlockID does not exist, or is not in correct state
    kBadBlockID = 4,
    // bad storage modification
    kStorageWriteFailed = 5,
    // Requesting an item that doesn't exist
    kNoStorageItem = 6,
    // tx validation or proposal failed
    kTransactionFailed = 7,
    // No next retrocausal queue item
    kNoRetrocausalQueueItem = 8,
    // Some error during validation, beyond just an error in the transaction,
    // such as a nondeterministic RCQ consume not backed by a concurrent insert.
    kValidationFailure = 9,
  };

  enum RPCFlags {
    kNone = 0,
    kFinalCommit = 0x00000001,
    kExclusiveLock = 0x00000002
  };

  typedef int RPCFlagSet;

  // Is this the XDR pattern we want?
  union ExpectedContract switch (RPCError error)
  {
  case kNone:
    Contract contract;
  default:
    void;
  };

  union ExpectedDeployment switch (RPCError error)
  {
  case kNone:
    SHA256Hash deployed_hash;
  default:
    void;
  };

  union ExpectedProposalData switch (RPCError error)
  {
  case kNone:
    NondeterministicProposal proposal_data;
  default:
    void;
  };

  typedef unsigned int ExecutorID;
  // First 32 bits of the TransactionID should be the ExecutorID.
  // Each executor can set the lowbits locally.
  typedef unsigned hyper TransactionID;

  union ExpectedStorageItem switch (RPCError error)
  {
  case kNone:
    StorageItem item;
  default:
    void;
  };

  union ExpectedRetrocausalQueueBlock switch (RPCError error)
  {
  case kNone:
    RetrocausalQueueBlock block;
  default:
    void;
  };

  union ExpectedQueueEntry switch (RPCError error)
  {
  case kNone:
    QueueEntry entry;
  default:
    void;
  };

  const kDBToExecutorPortNum = 10079;
  const kDBToCoordPortNum = 10080;
  const kExecToCoordPortNum = 10081;
  const kCoordAnnouncePortNum = 10082;
} // namespace hogd
