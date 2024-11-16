
// clang-format off
%#include "types.hh"
%#include "contract.hh"

namespace hogd
{
  // clang-format on
  typedef opaque Calldata<>;

  typedef opaque ReturnBuffer<>;

  enum MethodSpecifierType {
    kDefaultUint32 = 0,
  };

  union MethodSpecifier switch (MethodSpecifierType method_type)
  {
  case kDefaultUint32:
    MethodID method_id_uint32;
  };

  struct Invocation {
    ContractAddress invoked_address;
    MethodSpecifier method;
    GasAmount gas_limit;
    Calldata calldata;
  };

  struct Transaction {
    Invocation invocation;
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
