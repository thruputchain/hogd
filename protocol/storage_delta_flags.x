namespace hogd {

enum StorageItemRPCFlags {
  kStorageNone = 0,
  // Should the modified item be exclusively locked, in addition to the
  // modification performed.
  kStorageExclusiveLock = 0x00000001
};

typedef int StorageItemRPCFlagSet;

} // namespace hogd
