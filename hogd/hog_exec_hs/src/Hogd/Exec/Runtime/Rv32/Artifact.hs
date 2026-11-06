-- | Port of @hogd/hog_exec/runtime/rv32/artifact.h@ / @artifact.cc@: the RV32
-- artifact v5 wire format, its validation, the gas-schedule digest, the
-- format fingerprint, and SHA-256 section commitments.
--
-- Layout (little-endian): @magic[8], format_version, header_bytes,
-- vm_profile_digest, flags, entry_abi_version, syscall_abi_version,
-- gas_schedule_version, gas_schedule_digest, memory_bytes, code_size,
-- initial_memory_size, initial_memory_start, sha256(code),
-- sha256(initial_memory)@, then exactly @code || initial_memory@ -- section
-- positions are derived from the canonical order, not stored. The stored
-- image is placed at data-space VA @initial_memory_start@; the runtime
-- zero-fills @[0, start)@ and @[start + size, memory_bytes)@.
module Hogd.Exec.Runtime.Rv32.Artifact
  ( -- * Wire constants
    kArtifactMagic
  , kArtifactFormatVersion
  , kArtifactHeaderBytes
  , kArtifactVmProfileDigest
  , kArtifactEntryAbiVersion
  , kArtifactSyscallAbiVersion
  , kArtifactFlagsRequiredValue
  , kGasTableVersion
  , kArtifactDigestBytes
  , kArtifactFormatVersionOffset
  , kArtifactHeaderSizeOffset
  , kArtifactVmProfileOffset
  , kArtifactFlagsOffset
  , kArtifactEntryAbiOffset
  , kArtifactSyscallAbiOffset
  , kArtifactGasVersionOffset
  , kArtifactGasDigestOffset
  , kArtifactMemoryBytesOffset
  , kArtifactCodeSizeOffset
  , kArtifactInitialMemorySizeOffset
  , kArtifactInitialMemoryStartOffset
  , kArtifactCodeDigestOffset
  , kArtifactInitialMemoryDigestOffset
  , kArtifactSectionDigestAlgorithm
  , kArtifactCanonicalSectionOrder
    -- * Gas schedule
  , gasScheduleDigest
  , kGasScheduleDigest
  , AcceptedGasSchedule (..)
  , kTransitionalGasTableVersion
  , kTransitionalGasScheduleDigest
  , kAcceptedGasSchedules
    -- * Format fingerprint
  , formatFingerprint
  , kArtifactFormatFingerprint
  , kAcceptedFormatFingerprints
    -- * Parsing
  , ArtifactError (..)
  , Artifact (..)
  , artifactDigest
  , parseArtifact
  ) where

import qualified Crypto.Hash as H
import qualified Data.ByteArray as BA
import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import Data.List (find, foldl')
import Data.Word (Word32, Word64)
import Hogd.Exec.Runtime.GasCostTable (SyscallGasCostEntry, syscallGasCost, syscallGasCostSchedule, syscallGasName)
import Hogd.Exec.Runtime.Rv32.Encoding
import Hogd.Exec.Runtime.Rv32.Program

-- | The UTF-8 bytes of @"GHRV32A1"@.
kArtifactMagic :: ByteString
kArtifactMagic = B.pack [0x47, 0x48, 0x52, 0x56, 0x33, 0x32, 0x41, 0x31]

kArtifactFormatVersion :: Word32
kArtifactFormatVersion = 5

kArtifactHeaderBytes :: Word32
kArtifactHeaderBytes = 128

kArtifactVmProfileDigest :: Word64
kArtifactVmProfileDigest = 0x55c53a685f0eb33d

-- | v3: the VM initializes sp (x2) to zero; the guest establishes its stack.
kArtifactEntryAbiVersion :: Word32
kArtifactEntryAbiVersion = 3

-- | v1: register-native syscall ABI (args in a0..a7, no memory frame).
kArtifactSyscallAbiVersion :: Word32
kArtifactSyscallAbiVersion = 1

kArtifactFlagsRequiredValue :: Word32
kArtifactFlagsRequiredValue = 0

-- | Version of the RV32 gas schedule (instruction + host-syscall costs).
-- v3 admitted canonical UNIMP as a deterministic invalid-instruction trap.
kGasTableVersion :: Word32
kGasTableVersion = 4

kArtifactDigestBytes :: Int
kArtifactDigestBytes = 32

kArtifactFormatVersionOffset, kArtifactHeaderSizeOffset, kArtifactVmProfileOffset, kArtifactFlagsOffset :: Int
kArtifactFormatVersionOffset = 8
kArtifactHeaderSizeOffset = 12
kArtifactVmProfileOffset = 16
kArtifactFlagsOffset = 24

kArtifactEntryAbiOffset, kArtifactSyscallAbiOffset, kArtifactGasVersionOffset, kArtifactGasDigestOffset :: Int
kArtifactEntryAbiOffset = 28
kArtifactSyscallAbiOffset = 32
kArtifactGasVersionOffset = 36
kArtifactGasDigestOffset = 40

kArtifactMemoryBytesOffset, kArtifactCodeSizeOffset, kArtifactInitialMemorySizeOffset, kArtifactInitialMemoryStartOffset :: Int
kArtifactMemoryBytesOffset = 48
kArtifactCodeSizeOffset = 52
kArtifactInitialMemorySizeOffset = 56
kArtifactInitialMemoryStartOffset = 60

kArtifactCodeDigestOffset, kArtifactInitialMemoryDigestOffset :: Int
kArtifactCodeDigestOffset = 64
kArtifactInitialMemoryDigestOffset = kArtifactCodeDigestOffset + kArtifactDigestBytes

kArtifactSectionDigestAlgorithm :: ByteString
kArtifactSectionDigestAlgorithm = "sha256"

kArtifactCanonicalSectionOrder :: ByteString
kArtifactCanonicalSectionOrder = "header|code|initial_memory"

------------------------------------------------------------------------------
-- Gas schedule

-- | FNV-1a digest of instruction and host-syscall costs
-- (@gas_schedule_digest@).
gasScheduleDigest :: Word64
gasScheduleDigest =
  let d0 = fnvDigestU64 fnvOffsetBasis (fromIntegral kGasTableVersion)
      d1 = fnvDigestName d0 "rv32-instructions"
      d2 = fnvDigestU64 d1 (fromIntegral kOpCount)
      d3 =
        foldl'
          ( \d (i, e) ->
              let a = fnvDigestU64 d i
                  b = fnvDigestU64 a (fromIntegral (fromEnum e.gasEntryOp))
                  c = fnvDigestU64 b e.gasEntryCost
               in fnvDigestName c e.gasEntryName
          )
          d2
          (zip [0 ..] instructionGasSchedule)
      d4 = fnvDigestName d3 "host-syscalls"
      d5 = fnvDigestU64 d4 (fromIntegral (length syscallGasCostSchedule))
   in foldl'
        ( \d (i, e) ->
            let a = fnvDigestU64 d i
                b = fnvDigestU64 a (syscallGasCost e)
             in fnvDigestName b (syscallGasName e)
        )
        d5
        (zip [0 ..] syscallGasCostSchedule)

-- | The current schedule digest, pinned (the test suite checks that
-- 'gasScheduleDigest' still computes it, as the C++ static_assert does).
kGasScheduleDigest :: Word64
kGasScheduleDigest = 0xa958d226df658598

data AcceptedGasSchedule = AcceptedGasSchedule
  { acceptedVersion :: !Word32
  , acceptedDigest :: !Word64
  }
  deriving (Eq, Show)

-- | The previous schedule, retained for artifact compatibility.
kTransitionalGasTableVersion :: Word32
kTransitionalGasTableVersion = 3

kTransitionalGasScheduleDigest :: Word64
kTransitionalGasScheduleDigest = 0x918317c21c46bb60

-- | Accepted artifact declarations, matched as (version, digest) pairs.
-- Execution always uses this build's current prices, including for
-- artifacts declaring a transitional schedule. Consensus-relevant: removing
-- an entry requires accounting for deployed artifacts.
kAcceptedGasSchedules :: [AcceptedGasSchedule]
kAcceptedGasSchedules =
  [ AcceptedGasSchedule kGasTableVersion kGasScheduleDigest
  , AcceptedGasSchedule kTransitionalGasTableVersion kTransitionalGasScheduleDigest
  ]

------------------------------------------------------------------------------
-- Format fingerprint

fpU64 :: Word64 -> ByteString -> Word64 -> Word64
fpU64 digest name value = fnvDigestU64 (fnvDigestName digest name) value

fpBytes :: Word64 -> ByteString -> ByteString -> Word64
fpBytes digest name bytes =
  B.foldl' fnvDigestByte (fnvDigestU64 (fnvDigestName digest name) (fromIntegral (B.length bytes))) bytes

-- | A string value: its length (without the C terminator) then its bytes.
fpString :: Word64 -> ByteString -> ByteString -> Word64
fpString = fpBytes

fpLayout :: Word64 -> ByteString -> ByteString -> Word64 -> Word64 -> Word64
fpLayout digest offsetName sizeName offset size = fpU64 (fpU64 digest offsetName offset) sizeName size

-- | @format_fingerprint(format_version, gas_version, gas_digest,
-- folds_fixed_bounds)@: the format constants and validation policies with an
-- explicit encoding. @format_version@ is a sensitivity-test hook (the domain
-- string is fixed); @folds_fixed_bounds@ reproduces fingerprints from before
-- code and memory bounds became chain configuration.
formatFingerprint :: Word32 -> Word32 -> Word64 -> Bool -> Word64
formatFingerprint formatVersion gasVersion gasDigest foldsFixedBounds =
  let steps :: [Word64 -> Word64]
      steps =
        [ (`fnvDigestName` "groundhog.rv32.artifact.format.v5")
        , \d -> fpBytes d "magic" kArtifactMagic
        , \d -> fpU64 d "format_version" (fromIntegral formatVersion)
        , \d -> fpU64 d "header_bytes" (fromIntegral kArtifactHeaderBytes)
        , \d -> fpU64 d "vm_profile_digest" kArtifactVmProfileDigest
        , \d -> fpU64 d "flags_required_value" (fromIntegral kArtifactFlagsRequiredValue)
        , \d -> fpU64 d "entry_abi_version" (fromIntegral kArtifactEntryAbiVersion)
        , \d -> fpU64 d "syscall_abi_version" (fromIntegral kArtifactSyscallAbiVersion)
        , \d -> fpU64 d "gas_table_version" (fromIntegral gasVersion)
        , \d -> fpU64 d "gas_schedule_digest" gasDigest
        , \d -> fpLayout d "header.magic.offset" "header.magic.size" 0 (fromIntegral (B.length kArtifactMagic))
        , \d -> fpLayout d "header.format_version.offset" "header.format_version.size" (off kArtifactFormatVersionOffset) 4
        , \d -> fpLayout d "header.header_size.offset" "header.header_size.size" (off kArtifactHeaderSizeOffset) 4
        , \d -> fpLayout d "header.vm_profile.offset" "header.vm_profile.size" (off kArtifactVmProfileOffset) 8
        , \d -> fpLayout d "header.flags.offset" "header.flags.size" (off kArtifactFlagsOffset) 4
        , \d -> fpLayout d "header.entry_abi.offset" "header.entry_abi.size" (off kArtifactEntryAbiOffset) 4
        , \d -> fpLayout d "header.syscall_abi.offset" "header.syscall_abi.size" (off kArtifactSyscallAbiOffset) 4
        , \d -> fpLayout d "header.gas_version.offset" "header.gas_version.size" (off kArtifactGasVersionOffset) 4
        , \d -> fpLayout d "header.gas_digest.offset" "header.gas_digest.size" (off kArtifactGasDigestOffset) 8
        , \d -> fpLayout d "header.memory_bytes.offset" "header.memory_bytes.size" (off kArtifactMemoryBytesOffset) 4
        , \d -> fpLayout d "header.code_size.offset" "header.code_size.size" (off kArtifactCodeSizeOffset) 4
        , \d -> fpLayout d "header.initial_memory_size.offset" "header.initial_memory_size.size" (off kArtifactInitialMemorySizeOffset) 4
        , \d -> fpLayout d "header.initial_memory_start.offset" "header.initial_memory_start.size" (off kArtifactInitialMemoryStartOffset) 4
        , \d -> fpLayout d "header.code_digest.offset" "header.code_digest.size" (off kArtifactCodeDigestOffset) digestBytes
        , \d -> fpLayout d "header.initial_memory_digest.offset" "header.initial_memory_digest.size" (off kArtifactInitialMemoryDigestOffset) digestBytes
        , \d -> fpString d "section_digest_algorithm" kArtifactSectionDigestAlgorithm
        , \d -> fpU64 d "digest_bytes" digestBytes
        , \d -> fpU64 d "instruction_bytes" (fromIntegral kInstructionBytes)
        ]
          ++ ( if foldsFixedBounds
                then
                  -- Older lowerers folded the then-fixed bounds; reproduce
                  -- those historical values, not this build's defaults.
                  [ \d -> fpU64 d "max_code_bytes" (64 * 1024)
                  , \d -> fpU64 d "max_memory_bytes" (64 * 1024)
                  ]
                else []
             )
          ++ [ \d -> fpU64 d "memory_bytes_nonzero" 1
             , \d -> fpU64 d "memory_bytes_le_max" 1
             , \d -> fpU64 d "code_size_nonzero" 1
             , \d -> fpU64 d "initial_memory_start_plus_size_le_memory_bytes" 1
             , \d -> fpU64 d "initial_memory_start_zero_when_empty" 1
             , \d -> fpString d "canonical_section_order" kArtifactCanonicalSectionOrder
             , \d -> fpU64 d "no_section_gaps_or_trailing_bytes" 1
             ]
   in foldl' (\d f -> f d) fnvOffsetBasis steps
  where
    off = fromIntegral :: Int -> Word64
    digestBytes = fromIntegral kArtifactDigestBytes :: Word64

-- | This build's fingerprint (pinned to 0x4171b4b8dc19b9ab in the tests).
kArtifactFormatFingerprint :: Word64
kArtifactFormatFingerprint = formatFingerprint kArtifactFormatVersion kGasTableVersion kGasScheduleDigest False

-- | The fingerprints a lowerer may report: this build's own, then the gas
-- table v3 generations with and without the folded fixed bounds.
kAcceptedFormatFingerprints :: [Word64]
kAcceptedFormatFingerprints =
  [ kArtifactFormatFingerprint
  , formatFingerprint kArtifactFormatVersion kTransitionalGasTableVersion kTransitionalGasScheduleDigest True
  , formatFingerprint kArtifactFormatVersion kTransitionalGasTableVersion kTransitionalGasScheduleDigest False
  ]

------------------------------------------------------------------------------
-- Parsing

-- | Every artifact rejection, in check order.
data ArtifactError
  = ArtifactTooSmall
  | ArtifactBadMagic
  | ArtifactUnsupportedFormatVersion
  | ArtifactBadHeaderSize
  | ArtifactUnsupportedVmProfile
  | ArtifactUnsupportedFlags
  | ArtifactUnsupportedEntryAbi
  | ArtifactUnsupportedSyscallAbi
  | ArtifactGasScheduleVersionMismatch
  | ArtifactGasScheduleDigestMismatch
  | ArtifactMemorySizeInvalid
  | ArtifactCodeSizeInvalid
  | ArtifactInitialMemoryTooLarge
  | ArtifactInvalidSectionLayout
  | ArtifactCodeDigestMismatch
  | ArtifactInitialMemoryDigestMismatch
  | -- | The header and sections are valid but the code does not translate
    -- (see "Hogd.Exec.Runtime.Rv32.Rv32Contract").
    ArtifactInvalidProgram
  deriving (Eq, Show, Enum, Bounded)

-- | A header-validated artifact (@ThreadedArtifactImage@): the code and
-- stored initial-memory sections (slices of the input) and the declared
-- placement. Instruction validation happens at preparation.
data Artifact = Artifact
  { artifactCode :: !ByteString
  , artifactInitialMemory :: !ByteString
  , artifactInitialMemoryStart :: !Word32
  -- ^ Data-space VA where the stored image is placed; @[0, start)@ is zero.
  , artifactMemoryBytes :: !Word32
  }
  deriving (Eq, Show)

-- | Canonical SHA-256 over one artifact section.
artifactDigest :: ByteString -> ByteString
artifactDigest bytes = BA.convert (H.hashWith H.SHA256 bytes)

-- | @parse_threaded_artifact@ (@artifact_image_checks@), the production
-- parse: header fields, caller limits, placement, exact section layout and
-- section digests, in that order; the first failing check is reported.
-- Instruction decoding is deferred to preparation
-- ('Hogd.Exec.Runtime.Rv32.Rv32Contract.createContract'), which reports
-- every program problem -- including misaligned code -- as
-- 'ArtifactInvalidProgram'.
--
-- @limits@ bounds the @code_size@ and @memory_bytes@ the header may declare:
-- the chain's configuration at attachment, 'ceilingLimits' for a deployed
-- contract.
parseArtifact :: Rv32Limits -> ByteString -> Either ArtifactError Artifact
parseArtifact limits bytes
  | B.length bytes < fromIntegral kArtifactHeaderBytes = Left ArtifactTooSmall
  | B.take 8 bytes /= kArtifactMagic = Left ArtifactBadMagic
  | u32 kArtifactFormatVersionOffset /= kArtifactFormatVersion = Left ArtifactUnsupportedFormatVersion
  | u32 kArtifactHeaderSizeOffset /= kArtifactHeaderBytes = Left ArtifactBadHeaderSize
  | loadU64Le bytes kArtifactVmProfileOffset /= kArtifactVmProfileDigest = Left ArtifactUnsupportedVmProfile
  | u32 kArtifactFlagsOffset /= kArtifactFlagsRequiredValue = Left ArtifactUnsupportedFlags
  | u32 kArtifactEntryAbiOffset /= kArtifactEntryAbiVersion = Left ArtifactUnsupportedEntryAbi
  | u32 kArtifactSyscallAbiOffset /= kArtifactSyscallAbiVersion = Left ArtifactUnsupportedSyscallAbi
  | otherwise =
      -- Accepted as a (version, digest) pair.
      case find ((== gasVersion) . acceptedVersion) kAcceptedGasSchedules of
        Nothing -> Left ArtifactGasScheduleVersionMismatch
        Just accepted
          | loadU64Le bytes kArtifactGasDigestOffset /= acceptedDigest accepted -> Left ArtifactGasScheduleDigestMismatch
          | otherwise -> sizes
  where
    u32 = loadU32Le bytes
    gasVersion = u32 kArtifactGasVersionOffset
    memoryBytes = u32 kArtifactMemoryBytesOffset
    codeSize = u32 kArtifactCodeSizeOffset
    initialMemorySize = u32 kArtifactInitialMemorySizeOffset
    initialMemoryStart = u32 kArtifactInitialMemoryStartOffset
    codeOffset = fromIntegral kArtifactHeaderBytes :: Int
    sizes
      -- memory_bytes == 0 is a valid degenerate (every data access faults);
      -- only the caller's upper bound is enforced.
      | memoryBytes > limits.maxMemoryBytes = Left ArtifactMemorySizeInvalid
      | codeSize == 0 || codeSize > limits.maxCodeBytes = Left ArtifactCodeSizeInvalid
      -- Fit checked with subtraction so start + size cannot wrap.
      | initialMemoryStart > memoryBytes || initialMemorySize > memoryBytes - initialMemoryStart =
          Left ArtifactInitialMemoryTooLarge
      -- Canonicality: an empty image has exactly one encoding.
      | initialMemorySize == 0 && initialMemoryStart /= 0 = Left ArtifactInitialMemoryTooLarge
      -- header || code || initial_memory, exactly (computed in Integer so no
      -- sum can wrap).
      | toInteger (B.length bytes)
          /= toInteger kArtifactHeaderBytes + toInteger codeSize + toInteger initialMemorySize =
          Left ArtifactInvalidSectionLayout
      | artifactDigest code /= digestAt kArtifactCodeDigestOffset = Left ArtifactCodeDigestMismatch
      | artifactDigest initialMemory /= digestAt kArtifactInitialMemoryDigestOffset =
          Left ArtifactInitialMemoryDigestMismatch
      | otherwise =
          Right
            Artifact
              { artifactCode = code
              , artifactInitialMemory = initialMemory
              , artifactInitialMemoryStart = initialMemoryStart
              , artifactMemoryBytes = memoryBytes
              }
    code = B.take (fromIntegral codeSize) (B.drop codeOffset bytes)
    initialMemory = B.drop (codeOffset + fromIntegral codeSize) bytes
    digestAt offset = B.take kArtifactDigestBytes (B.drop offset bytes)
