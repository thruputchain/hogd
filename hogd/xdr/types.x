namespace hogd {

typedef unsigned int RuntimeMemoryOffset; // uint32_t
typedef unsigned int RuntimeMemoryLength; // uint32_t

typedef unsigned hyper GasAmount; // uint64_t
typedef unsigned int MethodID;    // uint32_t

enum StorageObjectType {
  kBytestring = 0,
  kNonnegativeInt64 = 1,
  kBoundedHashset = 2
};

typedef opaque uint128_xdr[16];
typedef opaque uint256_xdr[32];

typedef opaque Ed25519Signature[64]; // ed25519 sig len is 512 bits
typedef opaque Ed25519PublicKey[32]; // ed25519 key len is 256 bits
typedef opaque Ed25519SecretKey[64]; // ed25519 secret key len is 64 bytes

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

typedef opaque SHA256Hash[32]; // 256 bit hash

typedef opaque ContractAddress[32]; // 256 bit contract addresses

typedef opaque StorageKey[32]; // arbitrary 32 bytes

const kMaxLogMessageLen = 1024;
const kMaxReturnBufferLen = 1024;
const kBytestringMaxLen = 4096;

// in global storage:
// [contract address + storage key] = 64 bytes
// Contracts are free to set their own conventions for keys.
struct AddressAndKey {
  ContractAddress address;
  StorageKey key;
};

} // namespace hogd
