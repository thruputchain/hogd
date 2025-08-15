// clang-format off
%#include "coord.hh"
%#include "types.hh"

namespace hogd {
  // clang-format on

  typedef opaque ValidatorID[32];

  typedef Ed25519PublicKey ValidatorPublicKey;
  typedef Ed25519SecretKey ValidatorSecretKey;

  struct ValidatorPublicInfo {
    ValidatorPublicKey public_key;
    ServerAddress address;
  };

  struct ValidatorPrivateInfo {
    ValidatorPublicInfo public_info;
    ValidatorSecretKey secret_key;
  };

  struct QuorumSet {
    unsigned int threshold;
    ValidatorID validators<>;
    QuorumSet inner_sets<>;
  };

  struct QuorumSetSnapshot {
    Timestamp timestamp;
    QuorumSet quorum_set;
  };

  struct SignedQuorumSetSnapshot {
    ValidatorPublicInfo signer;
    Signature signature;
    QuorumSetSnapshot quorum_set_snapshot;
  };

  struct ValidatorInfoSnapshot {
    Timestamp timestamp;
    ValidatorPublicInfo validator;
  };

  struct SignedValidatorInfoSnapshot {
    ValidatorPublicInfo signer;
    Signature signature;
    ValidatorInfoSnapshot validator_snapshot;
  };

  struct KnownQuorumSets {
    SignedQuorumSetSnapshot quorum_set_snapshots<>;
    SignedValidatorInfoSnapshot validator_snapshots<>;
  };

  program Discovery
  {
    version Discovery1
    {

      void discovery_null(void) = 0;

      KnownQuorumSets discovery_update_quorums(KnownQuorumSets) = 10;
    }
    = 1;
  }
  = 0x3eef0505;

} // namespace hogd
