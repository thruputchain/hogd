namespace hogd {

struct uint128_xdr {
  unsigned hyper highbits;
  unsigned hyper lowbits;
};

struct int128_xdr {
  hyper highbits;
  unsigned hyper lowbits;
};

// int64_t, Unix time as nanoseconds elapsed since
// January 1, 1970 UTC
typedef hyper Timestamp;

typedef unsigned int RuntimeMemoryOffset; // uint32_t
typedef unsigned int RuntimeMemoryLength; // uint32_t

// TODO(geoff): consider using a smaller type for GasAmount
typedef unsigned hyper GasAmount; // uint64_t
typedef unsigned hyper MethodID;  // uint64_t

// ed25519 sig len is 512 bits
typedef opaque Ed25519Signature[64]; // #[rust(newtype)]
// ed25519 key len is 256 bits
typedef opaque Ed25519PublicKey[32]; // #[rust(newtype)]
// ed25519 secret key len is 64 bytes
typedef opaque Ed25519SecretKey[64]; // #[rust(newtype)]

enum CryptographicScheme {
  Ed25519 = 0,
};

union Signature switch (CryptographicScheme scheme)
{
case Ed25519:
  Ed25519Signature ed25519;
};

union PublicKey switch (CryptographicScheme scheme)
{
case Ed25519:
  Ed25519PublicKey ed25519;
};

union SecretKey switch (CryptographicScheme scheme)
{
case Ed25519:
  Ed25519SecretKey ed25519;
};

union ExtensionPoint switch (int v)
{
case 0:
  void;
};

// 256 bit hash
typedef opaque SHA256Hash[32]; // #[rust(newtype)]
// 256 bit contract address
typedef opaque ContractAddress[32]; // #[rust(newtype)]
typedef ContractAddress ContractAddressBatch<>;
// arbitrary 32-byte key into contract storage
// This is the key type that contracts directly manipulate
typedef opaque StorageKeyContractData[32]; // #[rust(newtype)]

enum StorageKeyType {
  kStorageKeyTypeContractData = 0,
  kStorageKeyTypeFreeNativeTokenBalance = 1,
};

union StorageKey switch (StorageKeyType key_type)
{
case kStorageKeyTypeContractData:
  StorageKeyContractData contract_data_key;
case kStorageKeyTypeFreeNativeTokenBalance:
  void;
  // TODO(geoff): add other key types
};

const kMaxLogMessageLen = 1024;   // #[rust(const=u32)]
const kMaxReturnBufferLen = 1024; // #[rust(const=u32)]

const kTotalHogSupply = 62'500'000'000; // #[rust(const=u64)]
const kHogDivisibility = 10'000'000;    // #[rust(const=u32)]
// Each hog token is divisible into 10^7 units

typedef unsigned hyper ReplayNonce;

// in global storage:
// [contract address + storage key] = 64 bytes
// Contracts are free to set their own conventions for keys.
struct AddressAndKey {
  ContractAddress address;
  StorageKey key;
};

struct LogEntry {
  ContractAddress source_address;
  opaque message<kMaxLogMessageLen>;
};

} // namespace hogd
