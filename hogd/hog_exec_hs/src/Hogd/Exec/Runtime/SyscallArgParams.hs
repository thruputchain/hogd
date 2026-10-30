{-# LANGUAGE AllowAmbiguousTypes #-}
{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE TypeApplications #-}

-- | The table-like, memory-independent parts of
-- @hogd/hog_exec/runtime/syscall_arg_params.h@: how a by-value syscall
-- argument slot is validated, how many slots each kind of argument takes,
-- and the null sentinels of optional inputs.
--
-- Not ported here (they need contract memory and the alias guard, so they
-- belong with the syscall entry port): the @parse@ / @parse_mock@ of span,
-- reference and optional-pointer arguments (@check_user_span_argument@,
-- @check_user_argument@, @check_user_argument_mock@, @AliasGuard@,
-- @WritableOutput@).
module Hogd.Exec.Runtime.SyscallArgParams
  ( narrowSlot
  , narrowSlotMock
  , parseBoolSlot
  , parseBoolSlotMock
  , SyscallArgKind (..)
  , kNumInputs
  , kOptionalInputNullSentinel
  , kOptionalInputNullSentinelMock
  ) where

import Data.Bits (FiniteBits (..), complement, isSigned, shiftL, (.&.))
import Data.Word (Word32, Word64)

-- | @narrow_slot<T>(uint32_t)@: validate a 32-bit argument slot as an
-- encoding of the integral type @t@ (at most 32 bits wide; C++
-- static_asserts it, here it is an 'error').
--
-- * unsigned types narrower than 32 bits reject set upper bits;
-- * signed types narrower than 32 bits require sign extension;
-- * 32-bit types accept every bit pattern (signed ones reinterpret it).
narrowSlot :: forall t. (Integral t, FiniteBits t, Bounded t) => Word32 -> Maybe t
narrowSlot input
  | bits > 32 =
      error
        "by-value syscall arguments occupy a single 32-bit slot; pass wider \
        \types by reference (as a memory offset) instead"
  | isSigned (0 :: t) =
      if bits < 32
        then
          let lowBitsMask = (1 `shiftL` bits) - 1 :: Word32
              upperBitsMask = complement lowBitsMask
              signBit = 1 `shiftL` (bits - 1) :: Word32
              upperBits = input .&. upperBitsMask
              signSet = input .&. signBit /= 0
           in if (not signSet && upperBits /= 0) || (signSet && upperBits /= upperBitsMask)
                then Nothing
                else Just (fromIntegral input)
        else Just (fromIntegral input)
  | bits < 32 && toInteger input > toInteger (maxBound :: t) = Nothing
  | otherwise = Just (fromIntegral input)
  where
    bits = finiteBitSize (0 :: t)

-- | @narrow_slot_mock<T>(uint64_t)@: the native (test_ffi) entrypoint keeps
-- 64-bit slots; a value slot past @UINT32_MAX@ is rejected rather than
-- truncated, then 'narrowSlot' applies.
narrowSlotMock :: forall t. (Integral t, FiniteBits t, Bounded t) => Word64 -> Maybe t
narrowSlotMock input
  | input > fromIntegral (maxBound :: Word32) = Nothing
  | otherwise = narrowSlot @t (fromIntegral input)

-- | @SyscallArgParams<bool>::parse@: only 0 and 1 encode a bool.
parseBoolSlot :: Word32 -> Maybe Bool
parseBoolSlot input
  | input > 1 = Nothing
  | otherwise = Just (input == 1)

-- | @SyscallArgParams<bool>::parse_mock@: the same check on a 64-bit slot.
parseBoolSlotMock :: Word64 -> Maybe Bool
parseBoolSlotMock input
  | input > 1 = Nothing
  | otherwise = Just (input == 1)

-- | The shapes a syscall argument takes, by the C++ type of the inner
-- syscall's parameter (see gen_syscalls.py @gen_validate_arg@).
data SyscallArgKind
  = -- | @std::span<const std::byte>@ (input) / @std::span<std::byte>@
    -- (output): an (offset, length) pair into contract memory.
    ArgSpan
  | -- | @std::optional<std::span<...>>@: an optional span (the generator
    -- supports it; no syscall uses it today).
    ArgOptionalSpan
  | -- | @T@ by value: a bool, integer or enum in one 32-bit slot
    -- ('narrowSlot').
    ArgValue
  | -- | @const T&@ (input) / @T&@ (output): a trivially copyable value at a
    -- contract-memory offset.
    ArgRef
  | -- | @const T*@: an optional input at a contract-memory offset, null
    -- when the offset is 'kOptionalInputNullSentinel'.
    ArgOptionalRef
  deriving (Eq, Ord, Show, Enum, Bounded)

-- | @SyscallArgParams<...>::kNumInputs@: argument slots the kind consumes.
kNumInputs :: SyscallArgKind -> Int
kNumInputs = \case
  ArgSpan -> 2
  ArgOptionalSpan -> 2
  ArgValue -> 1
  ArgRef -> 1
  ArgOptionalRef -> 1

-- | The explicit null of an optional input on the rv32 path (@UINT32_MAX@;
-- 0 is a valid guest address).
kOptionalInputNullSentinel :: Word32
kOptionalInputNullSentinel = maxBound

-- | The explicit null of an optional input on the native mock path
-- (@UINT64_MAX@).
kOptionalInputNullSentinelMock :: Word64
kOptionalInputNullSentinelMock = maxBound
