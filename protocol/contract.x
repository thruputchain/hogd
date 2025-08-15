
// clang-format off
%#include "types.hh"

namespace hogd {
  // clang-format on

  typedef opaque ContractBlob<>;

  enum ContractType {
    kWasmi = 0,
    kNative = 1,
    kMockTest = 2,
  };

  union Contract switch (ContractType contract_type)
  {
  case kWasmi:
    ContractBlob wasmi_contract;
  case kNative:
    unsigned int native_contract_id;
  case kMockTest:
    unsigned int mock_contract_id;
  default:
    void;
  };

  struct ContractDeploymentHashPreimage {
    ContractAddress deployer_address;
    SHA256Hash nonce;
    SHA256Hash contract_hash;
  };

  // This type must be a valid "identifier" as defined in RFC 4506,
  // Section 6.2: "An identifier is a letter followed by an optional sequence
  // of letters, digits, or underbar ('_')."  It must also obey Section 6.4,
  // which lists keywords that may not be used as identifiers.
  typedef string Identifier<>;

  // This type must be a valid version number as defined at
  // https://semver.org/.  For example, "0.1.3" or "1.0.0-alpha+001".
  typedef string SemVer<>;

  struct ContractInterfaceIDPreimage {
    Identifier name;
    SemVer vers;
  };

  // This type is computed as SHA256(ContractInterfaceIDPreimage)[0..6],
  // concatenated with a two-byte "method id" defined by the author of
  // the contract interface
  typedef opaque ContractEntrypointID[8]; // #[rust(newtype)]

} // namespace hogd
