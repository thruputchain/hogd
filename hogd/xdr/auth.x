// clang-format off
%#include "types.hh"

namespace hogd
{
  // clang-format on

  struct AuthContext {
    ContractAddress address;
    // for now, I just leave as opaque
    opaque bytes<>;
  };

  enum AuthEntryFlags {
    kRequireAllConsumed = 1,
  };

  struct AuthEntryNode {
    AuthContext context;
    AuthEntryNode children<>;
  };

  struct AuthEntryMetadata {
    unsigned int auth_flags;
    unsigned hyper signature_nonce;
  };

  typedef opaque
      Witness<kMaxAuthWitnessLen>; // in default auth setup, this is a Signature

  struct AuthEntry {
    ContractAddress authorizing_address;
    AuthEntryNode nodes<>;
    AuthEntryMetadata metadata;
    Witness witnesses<kMaxAuthWitnessCount>;
  };

  struct AuthEntrySignaturePayload {
    SHA256Hash message; // by default: Hash(source transaction)
    ContractAddress authorizing_address;
    AuthEntryNode nodes<>;
    AuthEntryMetadata metadata;
  };
}
