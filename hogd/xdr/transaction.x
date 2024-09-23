
%#include "types.hh"

namespace hogd {

typedef opaque Calldata<>;

typedef opaque ReturnBuffer<>;
typedef unsigned int MethodID; // uint32_t
typedef unsigned hyper GasAmount; // uint64_t


struct Invocation
{
	ContractAddress invoked_address;
	MethodID method_name;
	Calldata calldata;
};

struct Transaction
{
	Invocation invocation;

	GasAmount gas_limit;

	Contract contracts_to_deploy<>;
};

// Deliberately leaving this SignedTransaction object
// (and all the other metadata)
// for a later PR.
struct SignedTransaction {
	Transaction tx;
};

} // namespace hogd

