namespace hogd {

typedef unsigned int uint32;
typedef int int32;

typedef unsigned hyper uint64;
typedef hyper int64;

typedef opaque uint128[16];
typedef opaque uint256[32];

typedef opaque Ed25519Signature[64]; // ed25519 sig len is 512 bits
typedef opaque Ed25519PublicKey[32]; // ed25519 key len is 256 bits
typedef opaque Ed25519SecretKey[64]; // ed25519 secret key len is 64 bytes

typedef opaque SHA256Hash[32]; // 256 bit hash

typedef opaque ContractAddress[32]; // 256 bit contract addresses

typedef opaque StorageKey[32]; // arbitrary 32 bytes.

// in global storage:
// [contract address + storage key] = 64 bytes
// Contracts are free to set their own conventions for keys.
typedef opaque AddressAndKey[64];

} // namespace hogd
