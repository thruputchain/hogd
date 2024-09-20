%#include "xdr/storage_delta.hh"
%#include "xdr/types.hh"

enum StorageRPCError {
	NONE = 0,
	WRONG_SHARD = 1,
	APPLY_FAILED = 2,
	WRONG_BLOCK_HEADER = 3,
	ADDRESS_AND_KEY_DOES_NOT_EXIST = 4,
};

union ExpectedStaticStorageObject switch(StorageRPCError error)
{
	case NONE:
		StaticStorageObject object;
	default:
		void;
};

struct StorageDeltaVec {
	AddressAndKey addr_key;
	StorageDelta deltas<>;
};

program StorageInterface {
	version StorageInterfaceV1 {

		// Try to commit a vector of StorageDeltas.
		// All of the deltas should belong to the server.
		// Arguments:
		//   Vector of StorageDeltas
		//   Expected SHA256 Hash of previous block header
		//   Transaction SHA256
		// Errors:
		//   WRONG_BLOCK_HEADER if incorrect expected header
		//   WRONG_SHARD if _any_ of the input deltas are to the wrong shard
		//   Then, APPLY_FAILED if _any_ of the deltas fail
		// No-op whenever error is not NONE
		StorageRPCError try_commit_deltas(StorageDeltaVec<>, SHA256Hash, SHA256Hash) = 0;

		// Rollback all of the StorageDeltas sent by a transaction in the current block.
		// Arguments:
		//   Expected SHA256Hash of previous block header
		//   Transaction SHA256
		// No-op whenever error is not NONE
		StorageRPCError rollback_transaction_deltas(SHA256Hash, SHA256Hash) = 1;
		
		// Commit all of the StorageDeltas sent by a transaction in the current block.
		// Arguments:
		//   Expected SHA256Hash of previous block header
		//   Transaction SHA256
		// No-op whenever error is not NONE
		StorageRPCError commit_transaction_deltas(SHA256Hash, SHA256Hash) = 2;


		// Read-only methods

		// Read a bounded-size object from storage.
		// Arguments:
		//   Expected SHA256Hash of previous block header
		//   Requested AddressAndKey pair
		ExpectedStaticStorageObject get_static_object(SHA256Hash, AddressAndKey) = 3;

	} = 1;
} = 0x11111113;