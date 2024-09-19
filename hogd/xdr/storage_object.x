
%#include "xdr/types.hh"

namespace hogd {

enum StorageObjectType
{
	BYTESTRING = 0x0000,
	// Other static types with read/write interface

	NONNEGATIVE_INT64 = 0x1000,
	NONNEGATIVE_INT128 = 0x1001,
	// Perhaps other bit widths, or just standardize on 128 or 256

	HASH_SET = 0x2000,

	// Design choice 1: Asset type (no set in set_add)
	// Problem: No natural de-confliction rule between a subtract with no set
	// and a set of small value
	ASSET_128 = 0x3000,

	DEFERRED_RESOLUTION_SET = 0x4000,
};

struct int64_set_add_t
{
	int64 set_value;
	int64 delta;
};

struct int128_set_add_t
{
	int128 set_value;
	int128 delta;
};

struct hashset_insert_delta_t
{
	uint64 index;
	Hash hash;
};

}

const BYTESTRING_MAX_LEN = 4096;
typedef opaque Bytestring<BYTESTRING_MAX_LEN>;

enum StorageDeltaType 
{
	DELETE_LAST = 0,
	BYTESTRING_SET = 0x1000,

	NONNEGATIVE_INT64_SET_ADD = 0x2000,
	NONNEGATIVE_INT128_SET_ADD = 0x2001,
	// ... others

	HASH_SET_INSERT = 0x3000,
	HASH_SET_CLEAR = 0x3001,
	HASH_SET_DELETE = 0x3002,

	ASSET_128_ADD = 0x4000,

	DEFERRED_RESOLUTION_INSERT = 0x5000,
};

union StorageDelta switch(StorageDeltaType type)
{
	case DELETE_LAST:
		void;
	case BYTESTRING_SET:
		Bytestring bytestring_set;

	case NONNEGATIVE_INT64_SET_ADD:
		int64_set_add_t int64_set_add;
	case NONNEGATIVE_INT128_SET_ADD:
		int128_set_add_t int128_set_add;

	case HASH_SET_INSERT:
		hashset_insert_delta_t hs_insert;
	case HASH_SET_CLEAR:
		uint64 hs_clear_threshold;
	case HASH_SET_DELETE:
		Hash hs_deleted_hash;

	case ASSET_128_ADD:
		int128 asset128_add_amount;

	case DEFERRED_RESOLUTION_INSERT:
		// Not obvious how to express in XDR nicely,
		// but some type that can contain
		// any other StorageDelta
};

struct HashSetEntry
{
	uint64 index;
	Hash hash;
	uint64 escrowed_coins;
};

struct DeferredResolutionSetEntry {
	StorageDelta delta;
	uint64 escrowed_coins;
};

// Not how this would be stored on disk, probably
// Materializing this could be expensive
struct StorageObject
{
	union switch (StorageObjectType type)
	{
	case BYTESTRING:
		Bytestring bytestring;

	case NONNEGATIVE_INT64:
		int64 nonnegative_int64;
	case NONNEGATIVE_INT128:
		int128 nonnegative_int128;
	// ... others

	case ASSET_128:
		int128 asset_128;

	case HASH_SET:
		HashSetEntry hashset_elements<>;

	case DEFERRED_RESOLUTION_SET:
		DeferredResolutionSetEntry deferred_entries<>;

	} body;

	uint64 escrowed_coins;
};

program StorageInterface {
	version StorageInterfaceV1 {

	} = 1;
} = 0x11111113;

