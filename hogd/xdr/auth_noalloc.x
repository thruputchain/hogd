// clang-format off

namespace hogd
{
// clang-format on

enum AuthEntryFlags {
  kRequireAllConsumed = 1,
};

struct AuthEntryMetadata {
  unsigned int auth_flags;
  unsigned hyper signature_nonce;
};

struct VerifyAuthArgs {
  AuthEntryMetadata auth_metadata;
  unsigned int witness_count;
};
} // namespace hogd
