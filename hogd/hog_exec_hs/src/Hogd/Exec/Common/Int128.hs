-- | 128-bit integers as the executor uses them (int128_t / int128_xdr),
-- represented as 'Integer' with explicit range handling.
module Hogd.Exec.Common.Int128
  ( int128Min
  , int128Max
  , inInt128
  , fromXdr128
  , toXdr128
  , addOverflows128
  , int128FromLE
  , int128ToLE
  , uint128Max
  ) where

import Data.Bits
import qualified Data.ByteString as B
import Data.Word
import Hogd.Xdr.Types (X_int128_xdr (..))

int128Min, int128Max, uint128Max :: Integer
int128Min = negate (2 ^ (127 :: Int))
int128Max = 2 ^ (127 :: Int) - 1
uint128Max = 2 ^ (128 :: Int) - 1

inInt128 :: Integer -> Bool
inInt128 x = x >= int128Min && x <= int128Max

fromXdr128 :: X_int128_xdr -> Integer
fromXdr128 x = (fromIntegral x.highbits `shiftL` 64) .|. fromIntegral x.lowbits

-- | Wraps modulo 2^128, as a C++ conversion would.
toXdr128 :: Integer -> X_int128_xdr
toXdr128 v =
  let w = v .&. uint128Max
   in X_int128_xdr
        { highbits = fromIntegral (fromIntegral (w `shiftR` 64) :: Word64)
        , lowbits = fromIntegral (w .&. 0xffffffffffffffff)
        }

-- | integer_add_overflow for int128_t.
addOverflows128 :: Integer -> Integer -> Bool
addOverflows128 a b = not (inInt128 (a + b))

-- | Decode a little-endian two's-complement int128 (guest memory layout).
int128FromLE :: B.ByteString -> Integer
int128FromLE bs =
  let u = B.foldr (\b acc -> (acc `shiftL` 8) .|. fromIntegral b) 0 bs
   in if u > int128Max then u - 2 ^ (128 :: Int) else u

int128ToLE :: Integer -> B.ByteString
int128ToLE v =
  let w = v .&. uint128Max
   in B.pack [fromIntegral (w `shiftR` (8 * i)) | i <- [0 .. 15]]
