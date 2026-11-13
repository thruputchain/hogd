-- | Error channels of the executor (hogd/hog_exec/common/status.h).
--
-- The C++ code layers @std::expected@ over four error enums.  Here each
-- channel is an 'Either' over the generated XDR enum:
--
--  * 'SystemError'  -- block-ending (Unrecoverable / Abandon / ValidationFailure)
--  * 'InvokeError'  -- outcome of one invocation
--  * 'HostFnError'  -- what a syscall body reports to the sandbox
--  * 'SyscallErrorCode' -- the contract-visible syscall return code
module Hogd.Exec.Common.Status
  ( SystemError
  , SystemStatus
  , pattern SysUnrecoverable
  , pattern SysAbandon
  , pattern SysValidationFailure
  , systemErrorFromRaw
  , systemToHostFn
  , systemToInvoke
  , InvokeError
  , InvokeStatus
  , HostFnError
  , HostFnStatus
  , SyscallStatus
  , InvokeResult (..)
  , invokeResultCompatible
  , addInvokeResult
  , isFrameSuccess
  , shouldFrameRevert
  , isRootSuccess
  , hasReadableProposalData
  , toFrameStatus
  , unrecoverable
  , module Hogd.Xdr.FfiEnums
  , module Hogd.Xdr.SyscallError
  ) where

import Data.Int (Int32)
import Data.Word (Word64)
import Hogd.Exec.Log (logError)
import Hogd.Xdr.FfiEnums
import Hogd.Xdr.SyscallError
import Hogd.Xdr.Transaction (InvocationFrameStatusCode, pattern Fault, pattern InvocationFrameStatusCode_Return)

type SystemError = SystemErrorCode

type SystemStatus a = Either SystemError a

pattern SysUnrecoverable :: SystemError
pattern SysUnrecoverable = SystemErrorCode_Unrecoverable

pattern SysAbandon :: SystemError
pattern SysAbandon = SystemErrorCode_Abandon

pattern SysValidationFailure :: SystemError
pattern SysValidationFailure = SystemErrorCode_ValidationFailure

-- | SystemError::from_raw_error_code.
systemErrorFromRaw :: Int32 -> IO SystemError
systemErrorFromRaw raw = case SystemErrorCode raw of
  e@SystemErrorCode_Unrecoverable -> pure e
  e@SystemErrorCode_Abandon -> pure e
  e@SystemErrorCode_ValidationFailure -> pure e
  _ -> do
    logError ("Unrecoverable: Unknown system error code: " ++ show raw)
    pure SystemErrorCode_Unrecoverable

-- | SystemError::to_host_fn_error.
systemToHostFn :: SystemError -> HostFnError
systemToHostFn = \case
  SystemErrorCode_Abandon -> HostFnErrorCode_Abandon
  SystemErrorCode_ValidationFailure -> HostFnErrorCode_ValidationFailure
  _ -> HostFnErrorCode_Unrecoverable

-- | SystemError::to_invoke_error.
systemToInvoke :: SystemError -> InvokeError
systemToInvoke = \case
  SystemErrorCode_Abandon -> InvokeErrorCode_Abandon
  SystemErrorCode_ValidationFailure -> InvokeErrorCode_ValidationFailure
  _ -> InvokeErrorCode_Unrecoverable

type InvokeError = InvokeErrorCode

type InvokeStatus a = Either InvokeError a

type HostFnError = HostFnErrorCode

type HostFnStatus a = Either HostFnError a

type SyscallStatus a = Either SyscallErrorCode a

unrecoverable :: Either SystemError a
unrecoverable = Left SysUnrecoverable

-- | Total result of one contract invocation.  A frame is successful
-- (non-reverting) if 'irResult' is 'Right'.
data InvokeResult = InvokeResult
  { irResult :: !(InvokeStatus ())
  , irGasConsumed :: !Word64
  }
  deriving (Eq, Show)

-- | We can chain one InvokeResult onto another if the first was successful,
-- or both indicate the same error.
invokeResultCompatible :: InvokeResult -> InvokeResult -> Bool
invokeResultCompatible a b =
  isFrameSuccess a || case (irResult a, irResult b) of
    (Left ea, Left eb) -> ea == eb
    _ -> False

-- | @operator+=@: accumulate gas and take the other's result.
addInvokeResult :: InvokeResult -> InvokeResult -> InvokeResult
addInvokeResult a b = InvokeResult (irResult b) (irGasConsumed a + irGasConsumed b)

isFrameSuccess :: InvokeResult -> Bool
isFrameSuccess r = either (const False) (const True) (irResult r)

shouldFrameRevert :: InvokeResult -> Bool
shouldFrameRevert = not . isFrameSuccess

isRootSuccess :: InvokeResult -> Bool
isRootSuccess = isFrameSuccess

hasReadableProposalData :: InvokeResult -> Bool
hasReadableProposalData r = case irResult r of
  Right () -> True
  Left e -> e == Deterministic

-- | Map the frame's deterministic result to a frame outcome: Return on a
-- successful frame, Fault on a guest trap / out-of-gas; SystemErrors on the
-- error channel.
toFrameStatus :: InvokeResult -> IO (SystemStatus InvocationFrameStatusCode, Word64)
toFrameStatus (InvokeResult res gas) = case res of
  Right () -> pure (Right InvocationFrameStatusCode_Return, gas)
  Left e -> case e of
    InvokeErrorCode_Unrecoverable -> pure (Left SysUnrecoverable, gas)
    Deterministic -> pure (Right Fault, gas)
    InvokeErrorCode_OutOfGas -> pure (Right Fault, gas)
    InvokeErrorCode_Abandon -> pure (Left SysAbandon, gas)
    InvokeErrorCode_ValidationFailure -> pure (Left SysValidationFailure, gas)
    _ -> do
      logError ("Unrecoverable: InvokeResult::to_frame_status got unknown InvokeErrorCode " ++ show e)
      pure (Left SysUnrecoverable, gas)
