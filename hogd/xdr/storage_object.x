% #include "types.hh"

    namespace hogd
{

  const kBytestringMaxLen = 4096;

  typedef opaque Bytestring<kBytestringMaxLen>;

  union StorageObject switch (StorageObjectType type)
  {
  case kBytestring:
    Bytestring bytestring;
  case kNonnegativeInt64:
    hyper int64;
  };

} // namespace hogd
