
% #include "types.hh"

    namespace hogd
{

  typedef opaque Calldata<>;

  typedef opaque ReturnBuffer<>;
  typedef unsigned int MethodID;    // uint32_t

  struct Invocation {
    ContractAddress invoked_address;
    MethodID method_name;
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
