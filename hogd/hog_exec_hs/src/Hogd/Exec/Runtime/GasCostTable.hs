-- | Port of @hogd/hog_exec/runtime/gas_cost_table.h@: the syscall gas
-- schedule.
--
-- RV32 artifacts commit to these: 'kSyscallGasCostSchedule' is folded into
-- @rv32::kGasScheduleDigest@ (hogd/hog_exec/runtime/rv32/artifact.h).
-- Changing any cost, name or order is a coordinated gas-schedule update.
--
-- All arithmetic is 'Word64' (@uint64_t@) and wraps exactly as the C++
-- does. The C++ cost functions @assert@ that each length is at most
-- 'kMaxCostBytes'; those asserts are compiled out of release builds
-- (@-DNDEBUG@, the production configuration). They are 'assert's here too,
-- which GHC likewise drops under optimization (@-fignore-asserts@, implied
-- by @-O@), so an optimized build prices an out-of-range length with the
-- same wrapping arithmetic a release C++ build does.
module Hogd.Exec.Runtime.GasCostTable
  ( -- * Flat costs
    kSyscallBaseCost
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
    -- * Hash geometry
  , kSHA256BlockBytes
  , kSHA256PadBytes
  , kSHA512BlockBytes
  , kSHA512PadBytes
  , kEd25519HashedPrefixBytes
  , kSHAKE256RateBytes
  , kSHAKE256PadBytes
  , kMlDsa65HashedPrefixBytes
  , kMaxCostBytes
    -- * Cost functions (@struct GasCost@)
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
    -- * The hashed schedule
  , SyscallGasCostEntry (..)
  , kSyscallGasCostSchedule
    -- * Gas accumulation (syscall_entry_outer.h)
  , saturatingAddGas
  ) where

import Control.Exception (assert)
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.Word (Word64)
import Hogd.Xdr.Types (GasAmount)

-- | Every syscall pays this: the crossing itself. The ecall instruction
-- already charges 1 gas, so this is the remainder.
kSyscallBaseCost :: Word64
kSyscallBaseCost = 50

-- | Per SHA-256 compression block (no separate per-call base).
kSHA256HashBlockCost :: Word64
kSHA256HashBlockCost = 22

-- | Per-call stream-table cost for SHA256Update and SHA256Copy, in addition
-- to compression blocks.
kSHA256StreamCost :: Word64
kSHA256StreamCost = 26

-- | ed25519 verification: base plus per SHA-512 block of the hashed message.
kVerifySignatureBaseCost :: Word64
kVerifySignatureBaseCost = 17750

kVerifySignatureBlockCost :: Word64
kVerifySignatureBlockCost = 131

-- | ML-DSA-65 (pure, empty context): base plus per SHAKE256 block.
kVerifySignatureMlDsa65BaseCost :: Word64
kVerifySignatureMlDsa65BaseCost = 46592

kVerifySignatureMlDsa65BlockCost :: Word64
kVerifySignatureMlDsa65BlockCost = 160

-- | Per-call NNInt / bytestring storage access, on top of the crossing and
-- any database load.
kStorageAccessCost :: Word64
kStorageAccessCost = 150

-- | One read from hog_db, charged wherever an item view loads (a
-- transaction's first touch of a key), as though it went to the database.
kDBRPCCost :: Word64
kDBRPCCost = 37300

-- | Bytestring copy pricing: per 512-byte block, a write moving the bytes
-- twice.
kBytestringBlockBytes :: Word64
kBytestringBlockBytes = 512

kBytestringWriteBlockCost :: Word64
kBytestringWriteBlockCost = 13

kBytestringReadBlockCost :: Word64
kBytestringReadBlockCost = 7

kNoSyscallCost :: Word64
kNoSyscallCost = 10

kGenericBadSyscallCost :: Word64
kGenericBadSyscallCost = 10

-- | Block geometry, fixed by the hash functions (static_asserted, not
-- hashed). Padding is a 0x80 byte plus a length field.
kSHA256BlockBytes :: Word64
kSHA256BlockBytes = 64

kSHA256PadBytes :: Word64
kSHA256PadBytes = 9

kSHA512BlockBytes :: Word64
kSHA512BlockBytes = 128

kSHA512PadBytes :: Word64
kSHA512PadBytes = 17

-- | ed25519 hashes R || A || message with SHA-512.
kEd25519HashedPrefixBytes :: Word64
kEd25519HashedPrefixBytes = 64

-- | SHAKE256 rate; its pad10*1 padding takes at least one byte.
kSHAKE256RateBytes :: Word64
kSHAKE256RateBytes = 136

kSHAKE256PadBytes :: Word64
kSHAKE256PadBytes = 1

-- | ML-DSA-65 absorbs tr || 0x00 || 0x00 || message.
kMlDsa65HashedPrefixBytes :: Word64
kMlDsa65HashedPrefixBytes = 64 + 2

-- | The largest length a cost function may be handed (debug assert only).
kMaxCostBytes :: Word64
kMaxCostBytes = 2 ^ (32 :: Int)

-- | @GasCost::sha256_blocks@: blocks SHA-256 compresses, padding included.
sha256Blocks :: Word64 -> Word64
sha256Blocks messageBytes =
  assert (messageBytes <= kMaxCostBytes) $
    (messageBytes + kSHA256PadBytes + kSHA256BlockBytes - 1) `quot` kSHA256BlockBytes

-- | @GasCost::sha512_blocks@.
sha512Blocks :: Word64 -> Word64
sha512Blocks messageBytes =
  assert (messageBytes <= kMaxCostBytes) $
    (messageBytes + kSHA512PadBytes + kSHA512BlockBytes - 1) `quot` kSHA512BlockBytes

-- | @GasCost::shake256_blocks@.
shake256Blocks :: Word64 -> Word64
shake256Blocks messageBytes =
  assert (messageBytes <= kMaxCostBytes) $
    (messageBytes + kSHAKE256PadBytes + kSHAKE256RateBytes - 1) `quot` kSHAKE256RateBytes

-- | @GasCost::sha256_hash@.
sha256Hash :: Word64 -> Word64
sha256Hash bytesLen = kSHA256HashBlockCost * sha256Blocks bytesLen

-- | @GasCost::sha256_update@: whole blocks only; a partial tail stays
-- buffered and is charged by whichever later call completes it.
sha256Update :: Word64 -> Word64
sha256Update bytesLen =
  assert (bytesLen <= kMaxCostBytes) $
    kSHA256StreamCost + (kSHA256HashBlockCost * (bytesLen `quot` kSHA256BlockBytes))

-- | @GasCost::bytestring_blocks@: rounded up, an empty bytestring costing
-- none.
bytestringBlocks :: Word64 -> Word64
bytestringBlocks bytesLen =
  assert (bytesLen <= kMaxCostBytes) $
    (bytesLen + kBytestringBlockBytes - 1) `quot` kBytestringBlockBytes

-- | @GasCost::bytestring_set@: charged on the value handed to a write.
bytestringSet :: Word64 -> Word64
bytestringSet bytesLen = kStorageAccessCost + (kBytestringWriteBlockCost * bytestringBlocks bytesLen)

-- | @GasCost::bytestring_get@: charged on the bytes that reach the guest.
bytestringGet :: Word64 -> Word64
bytestringGet bytesLen = kStorageAccessCost + (kBytestringReadBlockCost * bytestringBlocks bytesLen)

-- | @GasCost::verify_signature@ (ed25519).
verifySignature :: Word64 -> Word64
verifySignature messageLen =
  assert (messageLen <= kMaxCostBytes) $
    kVerifySignatureBaseCost
      + (kVerifySignatureBlockCost * sha512Blocks (messageLen + kEd25519HashedPrefixBytes))

-- | @GasCost::verify_signature_mldsa65@.
verifySignatureMlDsa65 :: Word64 -> Word64
verifySignatureMlDsa65 messageLen =
  assert (messageLen <= kMaxCostBytes) $
    kVerifySignatureMlDsa65BaseCost
      + (kVerifySignatureMlDsa65BlockCost * shake256Blocks (messageLen + kMlDsa65HashedPrefixBytes))

-- | One entry of the hashed schedule. @name@ holds the UTF-8 code units of
-- the C++ @u8"..."@ literal (all ASCII).
data SyscallGasCostEntry = SyscallGasCostEntry
  { name :: B.ByteString
  , cost :: Word64
  }
  deriving (Eq, Show)

-- | @kSyscallGasCostSchedule@: every constant the cost functions read, in
-- digest order. Syscalls not named here charge 'kSyscallBaseCost' only.
kSyscallGasCostSchedule :: [SyscallGasCostEntry]
kSyscallGasCostSchedule =
  [ e "SyscallBase" kSyscallBaseCost
  , e "SHA256HashBlock" kSHA256HashBlockCost
  , e "SHA256Stream" kSHA256StreamCost
  , e "VerifySignatureBase" kVerifySignatureBaseCost
  , e "VerifySignatureBlock" kVerifySignatureBlockCost
  , e "StorageAccess" kStorageAccessCost
  , e "DBRPC" kDBRPCCost
  , e "BytestringBlockBytes" kBytestringBlockBytes
  , e "BytestringWriteBlock" kBytestringWriteBlockCost
  , e "BytestringReadBlock" kBytestringReadBlockCost
  , e "NoSyscall" kNoSyscallCost
  , e "GenericBadSyscall" kGenericBadSyscallCost
  , e "VerifySignatureMlDsa65Base" kVerifySignatureMlDsa65BaseCost
  , e "VerifySignatureMlDsa65Block" kVerifySignatureMlDsa65BlockCost
  ]
  where
    e n c = SyscallGasCostEntry {name = BC.pack n, cost = c}

-- | @detail::saturating_add@ (hogd/hog_exec/runtime/syscall_entry_outer.h):
-- the sum, or @UINT64_MAX@ when it overflows. Used to add
-- 'kSyscallBaseCost' to a syscall body's charge.
saturatingAddGas :: GasAmount -> GasAmount -> GasAmount
saturatingAddGas a b =
  let s = a + b
   in if s < a then maxBound else s
