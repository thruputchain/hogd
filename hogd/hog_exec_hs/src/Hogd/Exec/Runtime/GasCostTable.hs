-- | Port of @hogd/hog_exec/runtime/gas_cost_table.h@: host-syscall gas costs.
--
-- RV32 artifacts commit to these: 'syscallGasCostSchedule' is folded into the
-- RV32 gas-schedule digest ("Hogd.Exec.Runtime.Rv32.Artifact") and pinned
-- there. Changing any cost, name or order is a coordinated gas-schedule
-- update. See the C++ header for how each constant was measured.
--
-- The C++ cost functions @assert(bytes <= kMaxCostBytes)@ in debug builds
-- only; the release behaviour (no check) is what is ported here.
module Hogd.Exec.Runtime.GasCostTable
  ( kSyscallBaseCost
  , kSHA256HashBlockCost
  , kSHA256StreamCost
  , kVerifySignatureBaseCost
  , kVerifySignatureBlockCost
  , kVerifySignatureMlDsa65BaseCost
  , kVerifySignatureMlDsa65BlockCost
  , kStorageAccessCost
  , kDBRPCCost
  , kBytestringBlockBytes
  , kBytestringWriteBlockCost
  , kBytestringReadBlockCost
  , kNoSyscallCost
  , kGenericBadSyscallCost
  , kSHA256BlockBytes
  , kSHA256PadBytes
  , kSHA512BlockBytes
  , kSHA512PadBytes
  , kEd25519HashedPrefixBytes
  , kSHAKE256RateBytes
  , kSHAKE256PadBytes
  , kMlDsa65HashedPrefixBytes
  , kMaxCostBytes
    -- * Computed costs (@struct GasCost@)
  , sha256Blocks
  , sha512Blocks
  , shake256Blocks
  , sha256Hash
  , sha256Update
  , bytestringBlocks
  , bytestringSet
  , bytestringGet
  , verifySignature
  , verifySignatureMlDsa65
    -- * Digest-covered schedule
  , SyscallGasCostEntry (..)
  , syscallGasCostSchedule
  ) where

import Data.Bits (shiftL)
import Data.ByteString (ByteString)
import Data.Word (Word64)

-- | Every syscall pays this: the crossing itself. The ecall instruction
-- already charges 1 gas, so this is the remainder.
kSyscallBaseCost :: Word64
kSyscallBaseCost = 50

-- | Per SHA-256 compression block.
kSHA256HashBlockCost :: Word64
kSHA256HashBlockCost = 22

-- | Per-call stream-table cost for SHA256Update and SHA256Copy.
kSHA256StreamCost :: Word64
kSHA256StreamCost = 26

-- | ed25519 verification.
kVerifySignatureBaseCost, kVerifySignatureBlockCost :: Word64
kVerifySignatureBaseCost = 17750
kVerifySignatureBlockCost = 131

-- | ML-DSA-65 (pure, empty context) verification.
kVerifySignatureMlDsa65BaseCost, kVerifySignatureMlDsa65BlockCost :: Word64
kVerifySignatureMlDsa65BaseCost = 46592
kVerifySignatureMlDsa65BlockCost = 160

-- | Per-call NNInt access cost.
kStorageAccessCost :: Word64
kStorageAccessCost = 150

-- | One read from hog_db, charged wherever an item view loads.
kDBRPCCost :: Word64
kDBRPCCost = 37300

-- | Bytestring copy pricing (per 512-byte block, write and read rates).
kBytestringBlockBytes, kBytestringWriteBlockCost, kBytestringReadBlockCost :: Word64
kBytestringBlockBytes = 512
kBytestringWriteBlockCost = 13
kBytestringReadBlockCost = 7

kNoSyscallCost, kGenericBadSyscallCost :: Word64
kNoSyscallCost = 10
kGenericBadSyscallCost = 10

-- | Block geometry, fixed by the hash functions (not hashed into the schedule).
kSHA256BlockBytes, kSHA256PadBytes, kSHA512BlockBytes, kSHA512PadBytes :: Word64
kSHA256BlockBytes = 64
kSHA256PadBytes = 9
kSHA512BlockBytes = 128
kSHA512PadBytes = 17

-- | ed25519 hashes R || A || message with SHA-512.
kEd25519HashedPrefixBytes :: Word64
kEd25519HashedPrefixBytes = 64

-- | ML-DSA-65 absorbs tr || 0x00 || 0x00 || message into SHAKE256.
kSHAKE256RateBytes, kSHAKE256PadBytes, kMlDsa65HashedPrefixBytes :: Word64
kSHAKE256RateBytes = 136
kSHAKE256PadBytes = 1
kMlDsa65HashedPrefixBytes = 64 + 2

-- | The largest length a cost function may be handed.
kMaxCostBytes :: Word64
kMaxCostBytes = 1 `shiftL` 32

sha256Blocks :: Word64 -> Word64
sha256Blocks messageBytes =
  (messageBytes + kSHA256PadBytes + kSHA256BlockBytes - 1) `div` kSHA256BlockBytes

sha512Blocks :: Word64 -> Word64
sha512Blocks messageBytes =
  (messageBytes + kSHA512PadBytes + kSHA512BlockBytes - 1) `div` kSHA512BlockBytes

shake256Blocks :: Word64 -> Word64
shake256Blocks messageBytes =
  (messageBytes + kSHAKE256PadBytes + kSHAKE256RateBytes - 1) `div` kSHAKE256RateBytes

sha256Hash :: Word64 -> Word64
sha256Hash bytesLen = kSHA256HashBlockCost * sha256Blocks bytesLen

-- | Whole blocks only: a partial tail stays buffered and is charged by
-- whichever later call completes it.
sha256Update :: Word64 -> Word64
sha256Update bytesLen = kSHA256StreamCost + kSHA256HashBlockCost * (bytesLen `div` kSHA256BlockBytes)

-- | Rounded up; an empty bytestring costs none.
bytestringBlocks :: Word64 -> Word64
bytestringBlocks bytesLen = (bytesLen + kBytestringBlockBytes - 1) `div` kBytestringBlockBytes

bytestringSet :: Word64 -> Word64
bytestringSet bytesLen = kStorageAccessCost + kBytestringWriteBlockCost * bytestringBlocks bytesLen

bytestringGet :: Word64 -> Word64
bytestringGet bytesLen = kStorageAccessCost + kBytestringReadBlockCost * bytestringBlocks bytesLen

verifySignature :: Word64 -> Word64
verifySignature messageLen =
  kVerifySignatureBaseCost
    + kVerifySignatureBlockCost * sha512Blocks (messageLen + kEd25519HashedPrefixBytes)

verifySignatureMlDsa65 :: Word64 -> Word64
verifySignatureMlDsa65 messageLen =
  kVerifySignatureMlDsa65BaseCost
    + kVerifySignatureMlDsa65BlockCost * shake256Blocks (messageLen + kMlDsa65HashedPrefixBytes)

-- | One digest-covered schedule entry. The name is the UTF-8 byte string of
-- the C++ @u8"..."@ literal.
data SyscallGasCostEntry = SyscallGasCostEntry
  { syscallGasName :: !ByteString
  , syscallGasCost :: !Word64
  }
  deriving (Eq, Show)

-- | Every constant the cost functions read, in @kSyscallGasCostSchedule@
-- order. Syscalls not named here charge 'kSyscallBaseCost' and nothing more.
syscallGasCostSchedule :: [SyscallGasCostEntry]
syscallGasCostSchedule =
  [ SyscallGasCostEntry "SyscallBase" kSyscallBaseCost
  , SyscallGasCostEntry "SHA256HashBlock" kSHA256HashBlockCost
  , SyscallGasCostEntry "SHA256Stream" kSHA256StreamCost
  , SyscallGasCostEntry "VerifySignatureBase" kVerifySignatureBaseCost
  , SyscallGasCostEntry "VerifySignatureBlock" kVerifySignatureBlockCost
  , SyscallGasCostEntry "StorageAccess" kStorageAccessCost
  , SyscallGasCostEntry "DBRPC" kDBRPCCost
  , SyscallGasCostEntry "BytestringBlockBytes" kBytestringBlockBytes
  , SyscallGasCostEntry "BytestringWriteBlock" kBytestringWriteBlockCost
  , SyscallGasCostEntry "BytestringReadBlock" kBytestringReadBlockCost
  , SyscallGasCostEntry "NoSyscall" kNoSyscallCost
  , SyscallGasCostEntry "GenericBadSyscall" kGenericBadSyscallCost
  , SyscallGasCostEntry "VerifySignatureMlDsa65Base" kVerifySignatureMlDsa65BaseCost
  , SyscallGasCostEntry "VerifySignatureMlDsa65Block" kVerifySignatureMlDsa65BlockCost
  ]
