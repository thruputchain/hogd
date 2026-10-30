-- | Port of @hogd/hog_exec/runtime/rv32/host.h@: trap kinds, step results,
-- the ecall argument block, host results and the guest-range predicate.
--
-- The production engine (@ThreadedMachine@) does not support
-- @HostStatus::kTrap@ on its ecall path (@apply_ecall@ aborts on it) and the
-- production driver (@Rv32Runtime::run_ecall_loop@) resolves host errors
-- itself instead of passing them back into the VM, so 'HostResult' has no
-- trap arm and 'Trap' no @kHostTrap@. Every trap reachable in production is
-- represented.
module Hogd.Exec.Runtime.Rv32.Host
  ( memoryRangeInBounds
  , Trap (..)
  , StepResult (..)
  , EcallArgs (..)
  , HostResult (..)
  , continueWith
  , hostGasConsumed
  ) where

import Data.Word (Word32, Word64)

-- | The single bounds predicate for guest and host views: the subtraction
-- form avoids @address + width@ overflow while allowing a zero-length range
-- that ends exactly at the end of memory.
memoryRangeInBounds :: Word64 -> Word32 -> Word64 -> Bool
memoryRangeInBounds memorySize address width =
  let addr = fromIntegral address :: Word64
   in addr <= memorySize && width <= memorySize - addr
{-# INLINE memoryRangeInBounds #-}

-- | VM trap classification (consensus-visible).
data Trap
  = TrapNone
  | TrapInvalidInstruction
  | TrapBadPc
  | TrapBadMemory
  | TrapBreakpoint
  | TrapOutOfGas
  deriving (Eq, Show, Enum, Bounded)

-- | Result of driving the machine. 'StepContinue' is only returned by the
-- single-step test API ('Hogd.Exec.Runtime.Rv32.Machine.step').
data StepResult
  = StepContinue
  | StepEcall
  | StepHalted
  | StepTrap
  deriving (Eq, Show, Enum, Bounded)

-- | At @ecall@, @a0@ holds the syscall number and @a1..a7@ up to seven
-- 32-bit argument words.
data EcallArgs = EcallArgs
  { a0 :: !Word32
  , a1 :: !Word32
  , a2 :: !Word32
  , a3 :: !Word32
  , a4 :: !Word32
  , a5 :: !Word32
  , a6 :: !Word32
  , a7 :: !Word32
  }
  deriving (Eq, Show)

-- | Result of one host ecall. Host gas is debited before the payload is
-- applied; if it exceeds the remaining gas the machine traps out of gas with
-- zero gas left and the payload is not applied (no @a0@ write).
data HostResult
  = -- | Resume the guest with @a0 = value@.
    HostContinue !Word32 !Word64
  | -- | Terminate the machine with a halt code (supported by both C++
    -- engines; the production driver never uses it).
    HostHalt !Word32 !Word64
  | -- | Host-reported exhaustion: gas saturates to zero and the machine traps
    -- 'TrapOutOfGas'. Guest-memory writes the host made persist.
    HostOutOfGas !Word64
  deriving (Eq, Show)

-- | @HostResult::continue_with(value, gas_consumed)@: the only result the
-- production ecall loop passes back into the machine.
continueWith :: Word32 -> Word64 -> HostResult
continueWith = HostContinue

hostGasConsumed :: HostResult -> Word64
hostGasConsumed = \case
  HostContinue _ g -> g
  HostHalt _ g -> g
  HostOutOfGas g -> g
