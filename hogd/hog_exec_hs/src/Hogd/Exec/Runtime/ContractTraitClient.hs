-- | Calling a contract trait method from a native contract
-- (runtime/contract_trait_client.h), plus the by-syscall-number front end
-- to 'nativeSyscall' that native contract code uses
-- (@NativeSyscallContext::syscall<SyscallNo::X>@).
module Hogd.Exec.Runtime.ContractTraitClient
  ( -- * Native syscalls
    syscallMetaFor
  , nscSyscall
  , zeroInvokeOutputs
    -- * Clients
  , invokeOptRaw
  , invokeOpt
  , invokeOptResult
  , makeInvocation
  ) where

import qualified Data.ByteString as B
import Data.Maybe (fromMaybe)
import Data.Word
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Log (logTrace)
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallEntry (InnerResult, SyscallMeta)
import Hogd.Exec.Runtime.SyscallHandler (getReturnBuffer)
import Hogd.Exec.Runtime.Types (MethodID)
import Hogd.Exec.Runtime.Syscalls (InvokeOutputs (..), syscallMetaOf, sysInvoke)
import Hogd.Xdr

-- | The metadata of a syscall the executor implements.
syscallMetaFor :: SyscallNo -> SyscallMeta
syscallMetaFor n@(SyscallNo i) =
  fromMaybe (error ("syscallMetaFor: no syscall " ++ show n)) (syscallMetaOf (fromIntegral i))

-- | @NativeSyscallContext::syscall<No>(...)@: the outputs keep their zero
-- values when the syscall disarmed them, as the caller's variables do in
-- C++.
nscSyscall :: NativeSyscallContext -> SyscallNo -> o -> (Word64 -> IO (InnerResult, o)) -> IO (HostFnStatus SyscallErrorCode, o)
nscSyscall ctx no zero body = do
  (r, outs) <- nativeSyscall ctx (syscallMetaFor no) zero body
  pure (r, fromMaybe zero outs)

-- | @CopyableInvokeOutputs{}@.
zeroInvokeOutputs :: InvokeOutputs
zeroInvokeOutputs = InvokeOutputs 0 0 (InvocationFrameStatusCode 0)

-- | @ContractTraitClient::invoke_opt@ on already-encoded calldata, with a
-- decoder for the (buffer, manually reverted) pair.  The child gets an
-- unbounded gas limit (the syscall clamps it to what this frame has left)
-- and no auth tokens.
--
-- Left: a host-level failure of the invoke.  Right (Left code): the invoke
-- was rejected before a frame ran, or the child faulted (no buffer).
-- Right (Right Nothing): the buffer did not decode.
invokeOptRaw
  :: NativeSyscallContext
  -> ContractAddress
  -> MethodID
  -> InvokeFlagSet
  -> Calldata
  -> (B.ByteString -> Bool -> IO (Maybe r))
  -> IO (HostFnStatus (SyscallStatus (Maybe r)))
invokeOptRaw ctx target method flags calldata decode = do
  let h = nscHandler ctx
  (r, outputs) <-
    nscSyscall ctx KInvoke zeroInvokeOutputs $ \g ->
      sysInvoke h g target calldata B.empty (method, maxBound, flags)
  case r of
    Left e -> do
      logTrace ("Invoke HostFnError: " ++ show e)
      pure (Left e)
    Right code
      | code /= KSyscallErrorCodeNone -> do
          logTrace ("Invoke SyscallStatus error: " ++ show code)
          pure (Right (Left code))
      | otherwise -> do
          rb <- getReturnBuffer h
          case rb of
            Left e -> pure (Left (systemToHostFn e))
            Right (Left e) -> do
              logTrace ("No return buffer available: " ++ show e)
              pure (Right (Left e))
            Right (Right buf)
              | fromIntegral (ioReturnBufferLen outputs) /= B.length buf -> do
                  logTrace ("Unrecoverable: Return buffer size mismatch: " ++ show (ioReturnBufferLen outputs) ++ " != " ++ show (B.length buf))
                  pure (Left HostFnErrorCode_Unrecoverable)
              | otherwise -> do
                  let manuallyReverted = ioFrameStatus outputs == Abort
                  v <- decode buf manuallyReverted
                  case v of
                    Nothing -> logTrace "Invoke Error: Decode Return Failed"
                    Just _ -> pure ()
                  pure (Right (Right v))

-- | invoke_opt for a method whose return is a plain value.
invokeOpt
  :: NativeSyscallContext
  -> ContractAddress
  -> ContractTrait
  -> B.ByteString
  -> InvokeFlagSet
  -> Calldata
  -> (B.ByteString -> Maybe r)
  -> IO (HostFnStatus (SyscallStatus (Maybe r)))
invokeOpt ctx target trait method flags calldata decodeOk =
  invokeOptRaw ctx target (traitMethodId trait method) flags calldata (decodeReturn decodeOk)

-- | invoke_opt for a Result-returning method.
invokeOptResult
  :: ExceptionSet e
  => NativeSyscallContext
  -> ContractAddress
  -> ContractTrait
  -> B.ByteString
  -> InvokeFlagSet
  -> Calldata
  -> (B.ByteString -> Maybe r)
  -> IO (HostFnStatus (SyscallStatus (Maybe (Result e r))))
invokeOptResult ctx target trait method flags calldata decodeOk =
  invokeOptRaw ctx target (traitMethodId trait method) flags calldata (decodeResultReturn decodeOk)

-- | @ContractTraitClient::make_invocation@.
makeInvocation :: ContractTrait -> ContractAddress -> B.ByteString -> Word64 -> Calldata -> Invocation
makeInvocation trait target method gas calldata =
  Invocation
    { invoked_address = target
    , entrypoint = traitEntrypoint trait method
    , gas_limit = gas
    , calldata = calldata
    }
