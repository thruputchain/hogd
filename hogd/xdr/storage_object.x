namespace hogd {

enum StorageObjectType { kBytestring = 0, kNonnegativeInt64 = 1 };

const kBytestringMaxLen = 4096;

typedef opaque Bytestring<kBytestringMaxLen>;

union StorageObject switch (StorageObjectType type)
{
case kBytestring:
  Bytestring bytestring;
};

} // namespace hogd
