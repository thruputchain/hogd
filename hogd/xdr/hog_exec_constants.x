namespace hogd {

/// Legacy default for `kConfigParamMaxSHA256BufferBytes`, retained for
/// configuration hashing. Incremental SHA-256 no longer enforces a byte cap.
const kSHA256BufferMaxSize = 4096;

/// Storage savepoints one transaction may hold when a configuration does not
/// state `kConfigParamMaxSavepointDepth`. Frozen: changing it changes the
/// rules for every configuration that omits the parameter.
const kMaxSavepointDepth = 1024;

/// Bytes of RV32 code an attached artifact may carry when a configuration
/// does not state `kConfigParamMaxRv32CodeBytes`.
const kDefaultMaxRv32CodeBytes = 262144;

/// Bytes of RV32 guest memory an attached artifact may declare when a
/// configuration does not state `kConfigParamMaxRv32MemoryBytes`. Frozen for
/// the same reason as `kMaxSavepointDepth`.
const kDefaultMaxRv32MemoryBytes = 65536;

/// Oldest and newest RV32 artifact format versions this node reads. They
/// bound what `kConfigParamMinRv32ArtifactVersion` and
/// `kConfigParamMaxRv32ArtifactVersion` may state. The oldest is also the
/// minimum in force for a configuration that does not state one.
const kMinRv32ArtifactVersion = 5;
const kMaxRv32ArtifactVersion = 6;

/// Newest RV32 artifact format version admitted when a configuration does not
/// state `kConfigParamMaxRv32ArtifactVersion`. Below what this node reads, so a
/// newer format is admitted only once a chain states a maximum that includes
/// it. Consensus-critical for the same reason as `kMaxSavepointDepth`.
const kDefaultMaxRv32ArtifactVersion = 5;

} // namespace hogd
