-- | Port of the parts of @hogd/base/xdr_util.h@ that hog_exec uses: the
-- @int128_xdr@ / @uint128_xdr@ conversions ('toXdr' / 'fromXdr' in C++),
-- the fixed-width row encoding persisted and hashed into the DbHash
-- (@int128_to_xdr_bytes@), and @show_byte_array@ for diagnostics.
--
-- Not ported (unused by hog_exec proper): @xdr_{le,be}_to_cpu@ /
-- @xdr_cpu_to_{le,be}@, and the @ParallelBatch@ helpers
-- (@parallel_batch_to_t@, @t_to_parallel_batch@,
-- @append_to_parallel_batch@), which only hog_exec's benchmarks call.
module Hogd.Base.XdrUtil
  ( int128ToXdr
  , int128FromXdr
  , word128ToXdr
  , word128FromXdr
  , kInt128XdrBytes
  , int128ToXdrBytes
  , int128FromXdrBytes
  , showByteArray
  , encodeHex
  , kNoBatchSizeLimit
  , kNumThreadsForParallelBatch
  ) where

import Data.Bits (shiftR, (.&.))
import qualified Data.ByteString as B
import Data.List (intercalate)
import Data.Word (Word64)
import Hogd.Base.Types
import Hogd.Xdr.Types (X_int128_xdr (..), X_uint128_xdr (..))

-- | @to_xdr(int128_t)@: @highbits = v >> 64@ (arithmetic), @lowbits = v &
-- UINT64_MAX@.
int128ToXdr :: Int128 -> X_int128_xdr
int128ToXdr v = let (h, l) = int128Limbs v in X_int128_xdr {highbits = h, lowbits = l}

-- | @from_xdr(int128_xdr)@: @(int128_t{highbits} << 64) + lowbits@. Total:
-- every limb pair names exactly one value.
int128FromXdr :: X_int128_xdr -> Int128
int128FromXdr x = int128FromLimbs x.highbits x.lowbits

-- | @to_xdr(uint128_t)@.
word128ToXdr :: Word128 -> X_uint128_xdr
word128ToXdr v = let (h, l) = word128Limbs v in X_uint128_xdr {highbits = h, lowbits = l}

-- | @from_xdr(uint128_xdr)@.
word128FromXdr :: X_uint128_xdr -> Word128
word128FromXdr x = word128FromLimbs x.highbits x.lowbits

-- | @sizeof(int128_xdr)@, pinned to 16 by xdr_util.h (#3070): the width of a
-- persisted NNInt128 / balance row.
kInt128XdrBytes :: Int
kInt128XdrBytes = 16

-- | @int128_to_xdr_bytes@: big-endian high limb then big-endian low limb,
-- exactly the XDR serialization of the 'X_int128_xdr' (#3070).
int128ToXdrBytes :: Int128 -> B.ByteString
int128ToXdrBytes v =
  let (h, l) = int128Limbs v
   in be64 (fromIntegral h) <> be64 l

-- | @int128_from_xdr_bytes@. The C++ takes a fixed 16-byte span; here a
-- buffer of any other length is 'Nothing'.
int128FromXdrBytes :: B.ByteString -> Maybe Int128
int128FromXdrBytes bs
  | B.length bs /= kInt128XdrBytes = Nothing
  | otherwise =
      let (hb, lb) = B.splitAt 8 bs
       in Just (int128FromLimbs (fromIntegral (getBe64 hb)) (getBe64 lb))

be64 :: Word64 -> B.ByteString
be64 w = B.pack [fromIntegral (w `shiftR` (8 * i) .&. 0xff) | i <- [7, 6 .. 0]]

getBe64 :: B.ByteString -> Word64
getBe64 = B.foldl' (\acc b -> acc * 256 + fromIntegral b) 0

-- | @show_byte_array(std::array<uint8_t, N>)@ as formatted in hogd's logs.
-- Both toolchains hogd builds with (libc++ 21, libstdc++ 15) define
-- @__cpp_lib_format_ranges >= 202207L@, so the array itself is formatted as
-- a range of integers: @[1, 2, 255]@. (The pre-ranges fallback,
-- @encode_hex@, is 'encodeHex'.)
showByteArray :: B.ByteString -> String
showByteArray bs = "[" ++ intercalate ", " (map show (B.unpack bs)) ++ "]"

-- | @encode_hex@ (hogd/base/string.cc): lower-case hex, two digits a byte.
encodeHex :: B.ByteString -> String
encodeHex = concatMap byte . B.unpack
  where
    byte b = [digit (b `shiftR` 4), digit (b .&. 0xf)]
    digit n = "0123456789abcdef" !! fromIntegral n

-- | @kNoBatchSizeLimit@ (@SIZE_MAX@): the object limit for decoding input
-- this node produced itself.
kNoBatchSizeLimit :: SizeT
kNoBatchSizeLimit = maxBound

-- | @kNumThreadsForParallelBatch@.
kNumThreadsForParallelBatch :: SizeT
kNumThreadsForParallelBatch = 20
