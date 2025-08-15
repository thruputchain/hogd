namespace hogd {

struct SoloConfig {
  ValidatorPublicInfo leader;
  ValidatorPublicInfo quorum<>;
};

typedef opaque SoloNodeID[32];

typedef hyper SoloSlotIndex;

typedef opaque SoloValue<>;

enum SoloStatementType {
  SoloStatementAlpha = 0,
  SoloStatementBeta = 1,
  SoloStatementGamma = 2,
};

// SoloStatement is an extension point
union SoloStatement switch (SoloStatementType statement_type)
{
case SoloStatementAlpha:
  void;
case SoloStatementBeta:
  void;
case SoloStatementGamma:
  void;
default:
  void;
};

struct SoloStatementSet {
  SoloStatement statements<>;
};

enum SoloValidationLevel {
  SoloInvalidValue = 0,
  SoloMaybeValidValue = 1,
  SoloFullyValidValue = 2,
};

struct SoloNomination {
  SoloSlotIndex slot_index;
  SoloValue value;
};

struct SoloSignedNomination {
  ValidatorPublicInfo signer;
  Signature signature;
  SoloNomination nomination;
};

struct SoloValidate {
  SoloSlotIndex slot_index;
  SoloValue value;
};

struct SoloSignedValidate {
  ValidatorPublicInfo signer;
  Signature signature;
  SoloValidate validate;
};

struct SoloExternalized {
  SoloSlotIndex slot_index;
  SoloValue value;
};

struct SoloSignedExternalized {
  ValidatorPublicInfo signer;
  Signature signature;
  SoloExternalized externalized;
};

// SoloLeader is the server component of the centralized SCP implementation.
program SoloLeader
{
  version SoloLeader1
  {

    void sololeader_null(void) = 0;

    void sololeader_nominate(SoloSignedNomination) = 10;
  }
  = 1;
}
= 0x8aa81222;

// SoloFollower is the client component of the centralized SCP implementation.
program SoloFollower
{
  version SoloFollower1
  {

    void solofollower_null(void) = 0;

    SoloValidationLevel solofollower_validate(SoloSignedValidate) = 10;

    void solofollower_externalized(SoloSignedExternalized) = 20;
  }
  = 1;
}
= 0x8aa81223;

} // namespace hogd
