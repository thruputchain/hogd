// clang-format off
%#include "types.hh"
%#include "auth_noalloc.hh"

namespace hogd
{
  // clang-format on

  struct AuthContext {
    ContractAddress address;
    // for now, I just leave as opaque
    opaque bytes<>;
  };

  struct AuthEntryNode {
    AuthContext context;
    AuthEntryNode children<>;
  };

  // in default auth setup, this is a Signature
  typedef opaque Witness<kMaxAuthWitnessLen>;

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
