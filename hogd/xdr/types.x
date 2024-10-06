namespace hogd {

typedef opaque uint128_xdr[16];
typedef opaque uint256_xdr[32];

typedef opaque Ed25519Signature[64]; // ed25519 sig len is 512 bits
typedef opaque Ed25519PublicKey[32]; // ed25519 key len is 256 bits
typedef opaque Ed25519SecretKey[64]; // ed25519 secret key len is 64 bytes

enum CryptographicScheme {
  Ed25519 = 0,
};

union Signature switch (CryptographicScheme type)
{
case Ed25519:
  Ed25519Signature ed25519_signature;
};

union PublicKey switch (CryptographicScheme type)
{
case Ed25519:
  Ed25519PublicKey ed25519_publickey;
};

union SecretKey switch (CryptographicScheme type)
{
case Ed25519:
  Ed25519SecretKey ed25519_secretkey;
};

union ExtensionPoint switch (int v)
{
case 0:
  void;
};

typedef opaque SHA256Hash[32]; // 256 bit hash

typedef opaque ContractAddress[32]; // 256 bit contract addresses

typedef opaque StorageKey[32]; // arbitrary 32 bytes.

// in global storage:
// [contract address + storage key] = 64 bytes
// Contracts are free to set their own conventions for keys.
struct AddressAndKey {
  ContractAddress address;
  StorageKey key;
};

typedef opaque ContractBlob<>;

struct Contract {
  ContractBlob contract_data;
  // metadata for a future PR
};

} // namespace hogd
