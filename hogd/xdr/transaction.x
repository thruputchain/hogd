
// clang-format off
%#include "types.hh"
%#include "contract.hh"

namespace hogd
{
  // clang-format on
  typedef opaque Calldata<>;

  typedef opaque ReturnBuffer<>;

  struct Invocation {
    ContractAddress invoked_address;
    unsigned int method;
    GasAmount gas_limit;
    Calldata calldata;
  };

  struct Transaction {
    Invocation invocation;
    unsigned hyper nonce; // uint64_t
    Contract contracts_to_deploy<>;
  };

  // Deliberately leaving this SignedTransaction object
  // (and all the other metadata)
  // for a later PR.
  struct SignedTransaction {
    Transaction tx;
  };

  struct LogEntry {
    ContractAddress source_address;
    opaque message<kMaxLogMessageLen>;
  };

} // namespace hogd
