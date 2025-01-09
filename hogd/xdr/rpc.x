// clang-format off

%#include "types.hh"
%#include "contract.hh"

namespace hogd
{
  // clang-format on

  // This is obviously missing error codes.  Please expand or change as you
  // wish.
  enum StorageRPCError {
    kNone = 0,
    // Lookup on contract that does not exist in database
    kContractNExist = 1,
    // Lookup deployment of contract on address that has no deployed contract
    kNoContractDeployed = 2,
  };

  // Is this the XDR pattern we want?
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

  // Contract deploy and create are methods that cannot have conflicts,
  // and can only fail if there are catastrophic errors (like OOM) that would
  // cause us to abort the whole block.  As such, _at the moment_, they do not
  // have a 2PC interface, nor a TXID, although we could add those if we find a
  // reason to do so.
  program ContractStorageInterface
  {
    version ContractStorageInterfaceV1
    {
      // Create a new contract.
      // Arguments:
      //   New Contract
      // No-op whenever error is not kNone
      StorageRPCError commit_contract_create(Contract) = 1;

      // Create a new deployment.
      // Arguments:
      //   ContractAddress
      //   SHA256Hash of contract to be deployed
      // No-op whenever error is not kNone
      StorageRPCError commit_contract_deploy(ContractAddress, SHA256Hash) = 2;

      // TODO(geoff) consider whether we want to combine these or not.
      // Tradeoff is messages vs maybe sending redundant contracts.
      // Perhaps the sender could cache whether it expects the client to have
      // the contract, and optionally send accordingly.
      ExpectedContract get_contract(SHA256Hash) = 3;
      ExpectedDeployment get_deployment(ContractAddress) = 4;
    }
    = 1;
  }
  = 11111111;
} // namespace hogd
