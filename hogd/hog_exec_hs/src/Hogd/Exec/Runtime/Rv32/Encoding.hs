-- | Port of @hogd/hog_exec/runtime/rv32/encoding.h@: explicit little-endian
-- field access and the FNV-1a folding used by the RV32 artifact wire
-- constants (gas-schedule digest, format fingerprint).
module Hogd.Exec.Runtime.Rv32.Encoding
  ( fnvOffsetBasis
  , fnvPrime
  , loadU32Le
  , loadU64Le
  , putU32Le
  , putU64Le
  , fnvDigestByte
  , fnvDigestU64
  , fnvDigestName
  ) where

import Data.Bits (shiftL, shiftR, xor, (.|.))
import qualified Data.ByteString as B
import qualified Data.ByteString.Unsafe as BU
import Data.Word (Word32, Word64, Word8)

fnvOffsetBasis :: Word64
fnvOffsetBasis = 1469598103934665603

fnvPrime :: Word64
fnvPrime = 1099511628211

-- | Little-endian u32 at @offset@. The caller guarantees @offset + 4 <= len@
-- (as in the C++, which indexes a span the parser already sized).
loadU32Le :: B.ByteString -> Int -> Word32
loadU32Le bytes offset =
  fromIntegral (BU.unsafeIndex bytes offset)
    .|. (fromIntegral (BU.unsafeIndex bytes (offset + 1)) `shiftL` 8)
    .|. (fromIntegral (BU.unsafeIndex bytes (offset + 2)) `shiftL` 16)
    .|. (fromIntegral (BU.unsafeIndex bytes (offset + 3)) `shiftL` 24)
{-# INLINE loadU32Le #-}

-- | Little-endian u64 at @offset@; same precondition as 'loadU32Le'.
loadU64Le :: B.ByteString -> Int -> Word64
loadU64Le bytes offset =
  foldr
    (\i acc -> acc .|. (fromIntegral (BU.unsafeIndex bytes (offset + i)) `shiftL` (i * 8)))
    0
    [0 .. 7]

putU32Le :: Word32 -> B.ByteString
putU32Le v = B.pack [fromIntegral (v `shiftR` (i * 8)) | i <- [0 .. 3]]

putU64Le :: Word64 -> B.ByteString
putU64Le v = B.pack [fromIntegral (v `shiftR` (i * 8)) | i <- [0 .. 7]]

fnvDigestByte :: Word64 -> Word8 -> Word64
fnvDigestByte digest value = (digest `xor` fromIntegral value) * fnvPrime
{-# INLINE fnvDigestByte #-}

-- | Folds the eight little-endian bytes of @value@.
fnvDigestU64 :: Word64 -> Word64 -> Word64
fnvDigestU64 digest0 value0 = go (0 :: Int) digest0 value0
  where
    go 8 d _ = d
    go i d v = go (i + 1) (fnvDigestByte d (fromIntegral v)) (v `shiftR` 8)

-- | Folds a name's UTF-8 code units followed by a terminating zero byte, as
-- @fnv_digest_name@ does for its @u8"..."@ C string. Names are the UTF-8 bytes
-- of the C++ literals (all ASCII), passed here as a 'B.ByteString'.
fnvDigestName :: Word64 -> B.ByteString -> Word64
fnvDigestName digest name = fnvDigestByte (B.foldl' fnvDigestByte digest name) 0
