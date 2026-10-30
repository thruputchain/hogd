{-# LANGUAGE NoFieldSelectors #-}

-- | The data part of @hogd/hog_exec/runtime/syscall_types.h@.
--
-- Not ported here: @SyscallInnerResult@ /
-- @MaybeAsyncSyscallInnerResult@ (they pair a @HostFnStatus@ from
-- hog_exec/common/status.h, which belongs to the syscall handler port) and
-- @report_dropped_syscall_trace@ (a stderr diagnostic).
module Hogd.Exec.Runtime.SyscallTypes
  ( WrappedGasAmount (..)
  ) where

import Hogd.Xdr.Types (GasAmount)

-- | @WrappedGasAmount@: a gas amount passed to or returned from an inner
-- syscall body, wrapped so it cannot be confused with another integer.
newtype WrappedGasAmount = WrappedGasAmount {get :: GasAmount}
  deriving (Eq, Ord, Show)
