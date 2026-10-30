{-# LANGUAGE AllowAmbiguousTypes #-}
{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE TypeApplications #-}

-- | Port of @hogd/base/types.h@, plus the overflow helpers of
-- @hogd/base/compiler.h@ that the executor's int128 arithmetic is written in.
--
-- = 128-bit integers
--
-- C++ @int128_t@ / @uint128_t@ (@__int128@) are 'Int128' / 'Word128'. Both
-- are newtypes over 'Integer' whose value is always kept inside the type's
-- range; the constructors are not exported. Every arithmetic operation of
-- the 'Num' / 'Bits' instances wraps modulo 2^128 (two's complement for
-- 'Int128'), which is what the C++ code compiles to: unsigned arithmetic is
-- modular by definition, and signed @__int128@ overflow -- formally UB --
-- is what GCC and Clang emit (no trapping). Code that must /detect/
-- overflow, as hog_exec does with @__builtin_add_overflow@
-- (@integer_add_overflow@), uses 'integerAddOverflow' and friends, which
-- reproduce the builtins exactly: the operation is done in infinite
-- precision and the result is then converted (wrapped) to the destination
-- type, the flag telling whether that conversion lost information.
--
-- Conversions between widths ('wrapIntegral', 'fromIntegral') are modular,
-- like C++20 integral conversions. 'quot' / 'rem' truncate toward zero like
-- C++ @/@ and @%@; 'div' / 'mod' are Haskell's flooring versions. Division
-- by zero throws 'DivideByZero', and @minBound \`quot\` (-1)@ throws
-- 'Overflow' (mirroring GHC's own 'Int64'; in C++ both are UB and the
-- executor never reaches them).
--
-- The guest-memory layout of @int128_t@ (little-endian two's complement,
-- pinned by the static_assert in types.h) is 'int128ToLE' / 'int128FromLE'.
-- The XDR representation (@int128_xdr@, see xdr_util.h) is in
-- "Hogd.Base.XdrUtil".
module Hogd.Base.Types
  ( -- * Integer aliases (types.h)
    SizeT
  , SSizeT
    -- * 128-bit integers
  , Int128
  , Word128
  , int128FromLimbs
  , int128Limbs
  , word128FromLimbs
  , word128Limbs
  , int128ToWord128
  , word128ToInt128
  , int128ToLE
  , int128FromLE
  , word128ToLE
  , word128FromLE
    -- * Integral conversions
  , wrapIntegral
  , checkedIntegral
  , fitsIn
    -- * Overflow-checked arithmetic (compiler.h / GCC builtins)
  , integerAddOverflow
  , integerSubOverflow
  , integerMulOverflow
  , integerAddOverflows
  , integerSubOverflows
  , overflowingAdd
  , overflowingSub
  , overflowingMul
  , wrappingAdd
  , wrappingSub
  , checkedAdd
  , checkedSub
  , checkedMul
    -- * Size literals
  , kiB
  , miB
  , giB
    -- * Nondeterministic values
  , Nondeterministic (..)
    -- * Protocol identifier types used throughout hog_exec
  , BlockID
  , TransactionID
  , BlockHeight
  , GasAmount
  , TimePoint
  , RuntimeMemoryLength
  , ContractAddress
  , SHA256Hash
  ) where

import Control.Exception (ArithException (..), throw)
import Data.Bits
import qualified Data.ByteString as B
import Data.Int (Int64)
import Data.Word (Word64)
import Hogd.Xdr.RpcTypes (BlockID, TransactionID)
import Hogd.Xdr.Types
  ( BlockHeight
  , ContractAddress
  , GasAmount
  , RuntimeMemoryLength
  , SHA256Hash
  , TimePoint
  )

-- | @size_t@. hogd requires a 64-bit @size_t@ (xdr_util.h static_asserts
-- it), so it is 'Word64' with modular arithmetic.
type SizeT = Word64

-- | @ssize_t@ (@std::make_signed_t<size_t>@).
type SSizeT = Int64

------------------------------------------------------------------------------
-- 128-bit integers

-- | C++ @int128_t@ (@__int128@): two's complement, wrapping. The wrapped
-- 'Integer' always lies in [-2^127, 2^127).
newtype Int128 = Int128 Integer
  deriving (Eq, Ord)

-- | C++ @uint128_t@ (@unsigned __int128@): modular. The wrapped 'Integer'
-- always lies in [0, 2^128).
newtype Word128 = Word128 Integer
  deriving (Eq, Ord)

two64, two127, two128 :: Integer
two64 = 2 ^ (64 :: Int)
two127 = 2 ^ (127 :: Int)
two128 = 2 ^ (128 :: Int)

wrapI128 :: Integer -> Int128
wrapI128 n =
  let m = n `mod` two128
   in Int128 (if m >= two127 then m - two128 else m)

wrapW128 :: Integer -> Word128
wrapW128 n = Word128 (n `mod` two128)

instance Show Int128 where
  showsPrec d (Int128 n) = showsPrec d n

instance Show Word128 where
  showsPrec d (Word128 n) = showsPrec d n

instance Bounded Int128 where
  minBound = Int128 (negate two127)
  maxBound = Int128 (two127 - 1)

instance Bounded Word128 where
  minBound = Word128 0
  maxBound = Word128 (two128 - 1)

instance Num Int128 where
  Int128 a + Int128 b = wrapI128 (a + b)
  Int128 a - Int128 b = wrapI128 (a - b)
  Int128 a * Int128 b = wrapI128 (a * b)
  negate (Int128 a) = wrapI128 (negate a)
  abs (Int128 a) = wrapI128 (abs a)
  signum (Int128 a) = Int128 (signum a)
  fromInteger = wrapI128

instance Num Word128 where
  Word128 a + Word128 b = wrapW128 (a + b)
  Word128 a - Word128 b = wrapW128 (a - b)
  Word128 a * Word128 b = wrapW128 (a * b)
  negate (Word128 a) = wrapW128 (negate a)
  abs = id
  signum (Word128 a) = Word128 (signum a)
  fromInteger = wrapW128

instance Real Int128 where
  toRational (Int128 a) = toRational a

instance Real Word128 where
  toRational (Word128 a) = toRational a

instance Enum Int128 where
  succ x
    | x == maxBound = errorWithoutStackTrace "Enum.succ{Int128}: tried to take `succ' of maxBound"
    | otherwise = x + 1
  pred x
    | x == minBound = errorWithoutStackTrace "Enum.pred{Int128}: tried to take `pred' of minBound"
    | otherwise = x - 1
  toEnum = fromIntegral
  fromEnum (Int128 a)
    | a < toInteger (minBound :: Int) || a > toInteger (maxBound :: Int) =
        errorWithoutStackTrace "Enum.fromEnum{Int128}: value out of Int range"
    | otherwise = fromInteger a
  enumFrom x = enumFromTo x maxBound
  enumFromThen x y = enumFromThenTo x y (if y >= x then maxBound else minBound)
  enumFromTo (Int128 a) (Int128 b) = map Int128 [a .. b]
  enumFromThenTo (Int128 a) (Int128 b) (Int128 c) = map Int128 [a, b .. c]

instance Enum Word128 where
  succ x
    | x == maxBound = errorWithoutStackTrace "Enum.succ{Word128}: tried to take `succ' of maxBound"
    | otherwise = x + 1
  pred x
    | x == minBound = errorWithoutStackTrace "Enum.pred{Word128}: tried to take `pred' of minBound"
    | otherwise = x - 1
  toEnum n
    | n < 0 = errorWithoutStackTrace "Enum.toEnum{Word128}: negative argument"
    | otherwise = fromIntegral n
  fromEnum (Word128 a)
    | a > toInteger (maxBound :: Int) =
        errorWithoutStackTrace "Enum.fromEnum{Word128}: value out of Int range"
    | otherwise = fromInteger a
  enumFrom x = enumFromTo x maxBound
  enumFromThen x y = enumFromThenTo x y (if y >= x then maxBound else minBound)
  enumFromTo (Word128 a) (Word128 b) = map Word128 [a .. b]
  enumFromThenTo (Word128 a) (Word128 b) (Word128 c) = map Word128 [a, b .. c]

instance Integral Int128 where
  toInteger (Int128 a) = a
  quotRem x@(Int128 a) y@(Int128 b)
    | b == 0 = throw DivideByZero
    | x == minBound && y == -1 = throw Overflow
    | otherwise = let (q, r) = quotRem a b in (Int128 q, Int128 r)
  divMod x@(Int128 a) y@(Int128 b)
    | b == 0 = throw DivideByZero
    | x == minBound && y == -1 = throw Overflow
    | otherwise = let (q, r) = divMod a b in (Int128 q, Int128 r)

instance Integral Word128 where
  toInteger (Word128 a) = a
  quotRem (Word128 a) (Word128 b)
    | b == 0 = throw DivideByZero
    | otherwise = let (q, r) = quotRem a b in (Word128 q, Word128 r)
  divMod = quotRem

instance Bits Int128 where
  Int128 a .&. Int128 b = Int128 (a .&. b)
  Int128 a .|. Int128 b = Int128 (a .|. b)
  xor (Int128 a) (Int128 b) = Int128 (xor a b)
  complement (Int128 a) = Int128 (complement a)
  shiftL (Int128 a) n = wrapI128 (shiftL a n)
  -- Arithmetic shift, as GCC/Clang implement >> on a signed __int128
  -- (xdr_util.h static_asserts int128_t{-1} >> 64 == -1).
  shiftR (Int128 a) n = Int128 (shiftR a n)
  shift x n
    | n >= 0 = shiftL x n
    | otherwise = shiftR x (negate n)
  rotate x n = word128ToInt128 (rotate (int128ToWord128 x) n)
  bitSize _ = 128
  bitSizeMaybe _ = Just 128
  isSigned _ = True
  testBit (Int128 a) i = i >= 0 && i < 128 && testBit a i
  bit i
    | i >= 0 && i < 128 = wrapI128 (bit i)
    | otherwise = 0
  popCount x = popCount (int128ToWord128 x)

instance FiniteBits Int128 where
  finiteBitSize _ = 128
  countLeadingZeros x = countLeadingZeros (int128ToWord128 x)
  countTrailingZeros x = countTrailingZeros (int128ToWord128 x)

instance Bits Word128 where
  Word128 a .&. Word128 b = Word128 (a .&. b)
  Word128 a .|. Word128 b = Word128 (a .|. b)
  xor (Word128 a) (Word128 b) = Word128 (xor a b)
  complement (Word128 a) = Word128 (two128 - 1 - a)
  shiftL (Word128 a) n = wrapW128 (shiftL a n)
  shiftR (Word128 a) n = Word128 (shiftR a n)
  shift x n
    | n >= 0 = shiftL x n
    | otherwise = shiftR x (negate n)
  rotate x n0 =
    let n = n0 `mod` 128
     in if n == 0 then x else shiftL x n .|. shiftR x (128 - n)
  bitSize _ = 128
  bitSizeMaybe _ = Just 128
  isSigned _ = False
  testBit (Word128 a) i = i >= 0 && i < 128 && testBit a i
  bit i
    | i >= 0 && i < 128 = Word128 (bit i)
    | otherwise = 0
  popCount (Word128 a) = popCount a

instance FiniteBits Word128 where
  finiteBitSize _ = 128
  countLeadingZeros (Word128 a) = go 127
    where
      go i
        | i < 0 = 128
        | testBit a i = 127 - i
        | otherwise = go (i - 1)
  countTrailingZeros (Word128 a)
    | a == 0 = 128
    | otherwise = go 0
    where
      go i
        | testBit a i = i
        | otherwise = go (i + 1)

-- | @(int128_t{high} << 64) + low@: the value whose high limb (bits 64..127,
-- signed) is @high@ and whose low limb is @low@.
int128FromLimbs :: Int64 -> Word64 -> Int128
int128FromLimbs high low = Int128 (toInteger high * two64 + toInteger low)

-- | @(static_cast<int64_t>(v >> 64), static_cast<uint64_t>(v & UINT64_MAX))@.
int128Limbs :: Int128 -> (Int64, Word64)
int128Limbs (Int128 a) = (fromInteger (a `shiftR` 64), fromInteger (a .&. (two64 - 1)))

-- | @(uint128_t{high} << 64) + low@.
word128FromLimbs :: Word64 -> Word64 -> Word128
word128FromLimbs high low = Word128 (toInteger high * two64 + toInteger low)

-- | @(static_cast<uint64_t>(v >> 64), static_cast<uint64_t>(v & UINT64_MAX))@.
word128Limbs :: Word128 -> (Word64, Word64)
word128Limbs (Word128 a) = (fromInteger (a `shiftR` 64), fromInteger (a .&. (two64 - 1)))

-- | @static_cast<uint128_t>(int128_t)@: reinterpret the bits.
int128ToWord128 :: Int128 -> Word128
int128ToWord128 (Int128 a) = wrapW128 a

-- | @static_cast<int128_t>(uint128_t)@: reinterpret the bits.
word128ToInt128 :: Word128 -> Int128
word128ToInt128 (Word128 a) = wrapI128 a

-- | The 16-byte object representation of an @int128_t@: little-endian two's
-- complement (types.h pins this layout; syscalls memcpy it to and from
-- guest memory).
int128ToLE :: Int128 -> B.ByteString
int128ToLE = word128ToLE . int128ToWord128

-- | Inverse of 'int128ToLE'. 'Nothing' unless given exactly 16 bytes.
int128FromLE :: B.ByteString -> Maybe Int128
int128FromLE bs = word128ToInt128 <$> word128FromLE bs

-- | Little-endian object representation of a @uint128_t@.
word128ToLE :: Word128 -> B.ByteString
word128ToLE (Word128 a) = B.pack [fromInteger ((a `shiftR` (8 * i)) .&. 0xff) | i <- [0 .. 15]]

-- | Inverse of 'word128ToLE'. 'Nothing' unless given exactly 16 bytes.
word128FromLE :: B.ByteString -> Maybe Word128
word128FromLE bs
  | B.length bs /= 16 = Nothing
  | otherwise = Just (Word128 (B.foldr' (\b acc -> acc * 256 + toInteger b) 0 bs))

------------------------------------------------------------------------------
-- Integral conversions

-- | A C++ integral conversion (@static_cast@): modular, so it can never
-- fail. For every fixed-width type here 'fromInteger' already wraps.
wrapIntegral :: forall b a. (Integral a, Num b) => a -> b
wrapIntegral = fromInteger . toInteger

-- | Whether the value of @a@ is representable in @b@.
fitsIn :: forall b a. (Integral a, Integral b, Bounded b) => a -> Bool
fitsIn x =
  let n = toInteger x
   in n >= toInteger (minBound :: b) && n <= toInteger (maxBound :: b)

-- | The value of @a@ as a @b@, or 'Nothing' when it does not fit.
checkedIntegral :: forall b a. (Integral a, Integral b, Bounded b) => a -> Maybe b
checkedIntegral x
  | fitsIn @b x = Just (fromInteger (toInteger x))
  | otherwise = Nothing

------------------------------------------------------------------------------
-- Overflow-checked arithmetic

overflowResult :: forall r. (Integral r, Bounded r) => Integer -> (Bool, r)
overflowResult n =
  ( n < toInteger (minBound :: r) || n > toInteger (maxBound :: r)
  , fromInteger n
  )

-- | @__builtin_add_overflow(a, b, &r)@ / @integer_add_overflow(a, b, &r)@
-- (compiler.h): the infinitely precise sum, wrapped to @r@, and whether the
-- wrap changed it. Operand types are independent of the result type, as
-- for the builtin.
integerAddOverflow :: forall r a b. (Integral a, Integral b, Integral r, Bounded r) => a -> b -> (Bool, r)
integerAddOverflow a b = overflowResult @r (toInteger a + toInteger b)

-- | @__builtin_sub_overflow(a, b, &r)@ / @integer_sub_overflow(a, b, &r)@.
integerSubOverflow :: forall r a b. (Integral a, Integral b, Integral r, Bounded r) => a -> b -> (Bool, r)
integerSubOverflow a b = overflowResult @r (toInteger a - toInteger b)

-- | @__builtin_mul_overflow(a, b, &r)@.
integerMulOverflow :: forall r a b. (Integral a, Integral b, Integral r, Bounded r) => a -> b -> (Bool, r)
integerMulOverflow a b = overflowResult @r (toInteger a * toInteger b)

-- | The two-argument @integer_add_overflow<R>(a, b)@ (compiler.h): whether
-- @a + b@ overflows @r@. C++ defaults @R@ to @decltype(a + b)@; here the
-- result type is always given explicitly (@integerAddOverflows \@Int128 a b@).
integerAddOverflows :: forall r a b. (Integral a, Integral b, Integral r, Bounded r) => a -> b -> Bool
integerAddOverflows a b = fst (integerAddOverflow @r a b)

-- | The two-argument @integer_sub_overflow<R>(a, b)@.
integerSubOverflows :: forall r a b. (Integral a, Integral b, Integral r, Bounded r) => a -> b -> Bool
integerSubOverflows a b = fst (integerSubOverflow @r a b)

-- | @overflowing_add(a, b)@ (compiler.h): the wrapped sum and whether it
-- overflowed.
overflowingAdd :: (Integral t, Bounded t) => t -> t -> (t, Bool)
overflowingAdd a b = swap (integerAddOverflow a b)

-- | @overflowing_sub(a, b)@.
overflowingSub :: (Integral t, Bounded t) => t -> t -> (t, Bool)
overflowingSub a b = swap (integerSubOverflow a b)

-- | @overflowing_mul(a, b)@.
overflowingMul :: (Integral t, Bounded t) => t -> t -> (t, Bool)
overflowingMul a b = swap (integerMulOverflow a b)

swap :: (a, b) -> (b, a)
swap (a, b) = (b, a)

-- | @wrapping_add(a, b)@: modulo 2^N. (Every 'Num' instance of a
-- fixed-width type here already wraps; this names the intent.)
wrappingAdd :: Num t => t -> t -> t
wrappingAdd = (+)

-- | @wrapping_sub(a, b)@.
wrappingSub :: Num t => t -> t -> t
wrappingSub = (-)

-- | The sum, or 'Nothing' when it overflows @t@.
checkedAdd :: (Integral t, Bounded t) => t -> t -> Maybe t
checkedAdd a b = case integerAddOverflow a b of
  (False, r) -> Just r
  _ -> Nothing

-- | The difference, or 'Nothing' when it overflows @t@.
checkedSub :: (Integral t, Bounded t) => t -> t -> Maybe t
checkedSub a b = case integerSubOverflow a b of
  (False, r) -> Just r
  _ -> Nothing

-- | The product, or 'Nothing' when it overflows @t@.
checkedMul :: (Integral t, Bounded t) => t -> t -> Maybe t
checkedMul a b = case integerMulOverflow a b of
  (False, r) -> Just r
  _ -> Nothing

------------------------------------------------------------------------------
-- Size literals

-- | @operator""_KiB@: @kib * (1UL << 10)@, in @size_t@ (wrapping).
kiB :: SizeT -> SizeT
kiB k = k * (1 `shiftL` 10)

-- | @operator""_MiB@.
miB :: SizeT -> SizeT
miB m = m * (1 `shiftL` 20)

-- | @operator""_GiB@.
giB :: SizeT -> SizeT
giB g = g * (1 `shiftL` 30)

------------------------------------------------------------------------------
-- Nondeterministic

-- | @Nondeterministic<T>@: marks a value that may differ between
-- validators, so uses of it can be audited.
newtype Nondeterministic a = Nondeterministic {getNondeterministic :: a}
  deriving (Eq, Ord, Show)
