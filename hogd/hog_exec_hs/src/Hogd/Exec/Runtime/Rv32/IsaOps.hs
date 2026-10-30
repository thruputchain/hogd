-- | Port of @hogd/hog_exec/runtime/rv32/isa_ops.h@: instruction-field decode
-- primitives and the arithmetic cases that need explicit unsigned operations
-- or RISC-V's defined results for division overflow and division by zero.
--
-- All register values are 'Word32' bit patterns; signed views go through
-- 'i32FromBits' (a two's complement reinterpretation, like @std::bit_cast@).
module Hogd.Exec.Runtime.Rv32.IsaOps
  ( i32FromBits
  , bitsFromI32
  , bits
  , signExtend
  , opcode
  , rdField
  , funct3
  , rs1Field
  , rs2Field
  , funct7
  , immI
  , immS
  , immB
  , immU
  , immJ
  , arithmeticShiftRight
  , mulLow
  , mulh
  , mulhsu
  , mulhu
  , divUnsigned
  , divSigned
  , remUnsigned
  , remSigned
  ) where

import Data.Bits (complement, shiftL, shiftR, xor, (.&.), (.|.))
import Data.Int (Int32, Int64)
import Data.Word (Word32, Word64, Word8)

i32FromBits :: Word32 -> Int32
i32FromBits = fromIntegral
{-# INLINE i32FromBits #-}

bitsFromI32 :: Int32 -> Word32
bitsFromI32 = fromIntegral
{-# INLINE bitsFromI32 #-}

-- | @width@ bits of @value@ starting at bit @low@.
bits :: Word32 -> Int -> Int -> Word32
bits value low width = (value `shiftR` low) .&. ((1 `shiftL` width) - 1)
{-# INLINE bits #-}

-- | Sign-extends the low @bitsCount@ bits of @value@.
signExtend :: Word32 -> Int -> Word32
signExtend value bitsCount =
  let signBit = 1 `shiftL` (bitsCount - 1) :: Word32
   in (value `xor` signBit) - signBit
{-# INLINE signExtend #-}

opcode :: Word32 -> Word32
opcode word = word .&. 0x7f
{-# INLINE opcode #-}

rdField :: Word32 -> Word8
rdField word = fromIntegral (bits word 7 5)
{-# INLINE rdField #-}

funct3 :: Word32 -> Word8
funct3 word = fromIntegral (bits word 12 3)
{-# INLINE funct3 #-}

rs1Field :: Word32 -> Word8
rs1Field word = fromIntegral (bits word 15 5)
{-# INLINE rs1Field #-}

rs2Field :: Word32 -> Word8
rs2Field word = fromIntegral (bits word 20 5)
{-# INLINE rs2Field #-}

funct7 :: Word32 -> Word8
funct7 word = fromIntegral (bits word 25 7)
{-# INLINE funct7 #-}

immI :: Word32 -> Int32
immI word = i32FromBits (signExtend (bits word 20 12) 12)

immS :: Word32 -> Int32
immS word = i32FromBits (signExtend ((bits word 25 7 `shiftL` 5) .|. bits word 7 5) 12)

immB :: Word32 -> Int32
immB word =
  let value =
        (bits word 31 1 `shiftL` 12)
          .|. (bits word 7 1 `shiftL` 11)
          .|. (bits word 25 6 `shiftL` 5)
          .|. (bits word 8 4 `shiftL` 1)
   in i32FromBits (signExtend value 13)

immU :: Word32 -> Int32
immU word = i32FromBits (word .&. 0xfffff000)

immJ :: Word32 -> Int32
immJ word =
  let value =
        (bits word 31 1 `shiftL` 20)
          .|. (bits word 12 8 `shiftL` 12)
          .|. (bits word 20 1 `shiftL` 11)
          .|. (bits word 21 10 `shiftL` 1)
   in i32FromBits (signExtend value 21)

-- | Arithmetic right shift; callers mask the amount to five bits.
arithmeticShiftRight :: Word32 -> Int -> Word32
arithmeticShiftRight value amount = bitsFromI32 (i32FromBits value `shiftR` amount)
{-# INLINE arithmeticShiftRight #-}

mulLow :: Word32 -> Word32 -> Word32
mulLow lhs rhs = lhs * rhs
{-# INLINE mulLow #-}

-- | High half of signed x signed.
mulh :: Word32 -> Word32 -> Word32
mulh lhs rhs =
  let product' = fromIntegral (i32FromBits lhs) * fromIntegral (i32FromBits rhs) :: Int64
   in fromIntegral ((fromIntegral product' :: Word64) `shiftR` 32)
{-# INLINE mulh #-}

-- | High half of signed x unsigned.
mulhsu :: Word32 -> Word32 -> Word32
mulhsu lhs rhs =
  let product' = fromIntegral (i32FromBits lhs) * (fromIntegral rhs :: Int64)
   in fromIntegral ((fromIntegral product' :: Word64) `shiftR` 32)
{-# INLINE mulhsu #-}

-- | High half of unsigned x unsigned.
mulhu :: Word32 -> Word32 -> Word32
mulhu lhs rhs =
  let product' = fromIntegral lhs * fromIntegral rhs :: Word64
   in fromIntegral (product' `shiftR` 32)
{-# INLINE mulhu #-}

-- | RISC-V: x / 0 = all ones.
divUnsigned :: Word32 -> Word32 -> Word32
divUnsigned lhs rhs
  | rhs == 0 = complement 0
  | otherwise = lhs `quot` rhs
{-# INLINE divUnsigned #-}

-- | RISC-V: x / 0 = -1; INT_MIN / -1 = INT_MIN (overflow). Truncates toward
-- zero like C++ @/@.
divSigned :: Word32 -> Word32 -> Word32
divSigned lhs rhs
  | divisor == 0 = complement 0
  | dividend == minBound && divisor == -1 = lhs
  | otherwise = bitsFromI32 (dividend `quot` divisor)
  where
    divisor = i32FromBits rhs
    dividend = i32FromBits lhs
{-# INLINE divSigned #-}

-- | RISC-V: x % 0 = x.
remUnsigned :: Word32 -> Word32 -> Word32
remUnsigned lhs rhs
  | rhs == 0 = lhs
  | otherwise = lhs `rem` rhs
{-# INLINE remUnsigned #-}

-- | RISC-V: x % 0 = x; INT_MIN % -1 = 0. Sign follows the dividend (C++ @%@).
remSigned :: Word32 -> Word32 -> Word32
remSigned lhs rhs
  | divisor == 0 = lhs
  | dividend == minBound && divisor == -1 = 0
  | otherwise = bitsFromI32 (dividend `rem` divisor)
  where
    divisor = i32FromBits rhs
    dividend = i32FromBits lhs
{-# INLINE remSigned #-}
