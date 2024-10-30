
// clang-format off

%#include "storage_object.hh" 
%#include "types.hh"
%#include "contract.hh"

namespace hogd
{
  // clang-format on

  enum StorageRPCError {
    kNone = 0,
    // Query went to the wrong database shard
    kWrongShard = 1,
    // try_reserve_delta failed on some delta.  The rpc should be a no-op
    kApplyFailed = 2,
    // Mismatch between the supplied expected block header and the actual header
    kOutOfDateBlockHeader = 3,
    // Lookup on key that does not exist in database
    kAddressAndKeyNExist = 4,
    // Lookup on contract that does not exist in database
    kContractNExist = 5,
    // Lookup deployment of contract on address that has no deployed contract
    kNoContractDeployed = 6,
  };

  struct StorageDeltaVec {
    AddressAndKey addr_key;
    StorageDelta deltas<>;
  };

  typedef StorageDeltaVec StorageDeltaVecList<>;

  union ExpectedStorageObject switch (StorageRPCError error)
  {
  case kNone:
    StrippedStorageObject object;
  default:
    void;
  };

  union ExpectedContract switch (StorageRPCError error)
  {
  case kNone:
    Contract contract;
  default:
    void;
  };

  union ExpectedDeployment switch (StorageRPCError error)
  {
  case kNone:
    SHA256Hash deployed_hash;
  default:
    void;
  };

  program StorageWriteInterface
  {
    version StorageWriteInterfaceV1
    {

      // The logical workflow of a StorageDelta is:
      // - try_commit(delta)
      // - if the transaction fails: revert(delta)
      // - if the transaction succees: commit(delta)
      // This is more messages than is necessary---some delta types
      // might be able to skip some of these steps.

      // Try to commit a vector of StorageDeltas.
      // All of the deltas should belong to the server.
      // Arguments:
      //   Vector of StorageDeltas
      //   Expected SHA256 Hash of previous block header
      // Errors:
      //   kOutOfDateBlockHeader if incorrect expected header
      //   kWrongShard if _any_ of the input deltas are to the wrong shard
      //   Then, kApplyFailed if _any_ of the deltas fail
      // No-op whenever error is not kNone
      StorageRPCError try_reserve_transaction_deltas(StorageDeltaVecList,
                                                     SHA256Hash) = 0;

      // Rollback all of the StorageDeltas sent by a transaction in the current
      // block. Arguments:
      //   Vector of StorageDeltas to be undone
      //   Expected SHA256Hash of previous block header
      // No-op whenever error is not kNone
      StorageRPCError revert_transaction_deltas(StorageDeltaVecList,
                                                SHA256Hash) = 1;

      // Commit all of the StorageDeltas sent by a transaction in the current
      // block. Arguments:
      //   StorageDeltas to be committed
      //   Expected SHA256Hash of previous block header
      // No-op whenever error is not kNone
      StorageRPCError commit_transaction_deltas(StorageDeltaVecList,
                                                SHA256Hash) = 2;

      // Create a new contract.
      // Arguments:
      //   New Contract
      //   Expected SHA256Hash of previous block header
      // No-op whenever error is not kNone
      StorageRPCError commit_contract_create(Contract, SHA256Hash) = 3;

      // Create a new deployment.
      // Arguments:
      //   ContractAddress
      //   SHA256Hash of contract to be deployed
      //   Expected SHA256Hash of previous block header
      // No-op whenever error is not kNone
      StorageRPCError commit_contract_deploy(ContractAddress, SHA256Hash,
                                             SHA256Hash) = 4;
    }
    = 1;
  }
  = 11111113;

  program StorageReadInterface
  {
    version StorageReadInterfaceV1
    {
      // Read-only methods

      // Read an object from storage.
      // Arguments:
      //   Expected SHA256Hash of previous block header
      //   Requested AddressAndKey pair
      ExpectedStorageObject get_object(SHA256Hash, AddressAndKey) = 0;

      // TODO(geoff) consider whether we want to combine these or not.
      // Tradeoff is messages vs maybe sending redundant contracts.
      // Perhaps the sender could cache whether it expects the client to have
      // the contract, and optionally send accordingly.
      ExpectedContract get_contract(SHA256Hash) = 1;
      ExpectedDeployment get_deployment(ContractAddress) = 2;
    }
    = 1;
  }
  = 11111114;
}
