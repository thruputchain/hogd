-- | Native (precompiled) contracts and the multiplexing runtime context
-- (runtime/native_runtime.*, native_syscall_context.*, multiplexed_context.*).
--
-- Native contracts run inside the trust boundary of the syscall handler.
-- They meter themselves through a 'NativeSyscallContext' and reach storage
-- through the same syscall bodies guest contracts use.
module Hogd.Exec.Runtime.NativeRuntime
  ( NativeSyscallContext
  , nscHandler
  , newNativeSyscallContext
  , nativeSyscall
  , nscGasRemaining
  , nscTotalGasConsumed
  , nscConsumeGas
  , nscNativeTokenBalance
  , nscModifyNativeTokenBalance
  , NativeFunction
  , NativeContract
  , mkNativeContract
  , nativeContractInvoke
  , nativeContext
  , multiplexedContext
  ) where

import Data.IORef
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Exec.Runtime.GasCostTable (kStorageAccessCost)
import Hogd.Exec.Runtime.Item.StorageSnapshotCache (DeployedContract (..))
import Hogd.Exec.Runtime.PreparedContract
import Hogd.Exec.Runtime.SyscallEntry
import Hogd.Exec.Runtime.SyscallHandler
import Hogd.Exec.Runtime.Types
import Hogd.Exec.Util (showByteArray)
import Hogd.Xdr.Contract
import Hogd.Xdr.Types (ContractAddress)

-- | Routes native contract syscalls through 'wrapAfterParse', so they get
-- read-only enforcement and gas accounting like guest contracts.
data NativeSyscallContext = NativeSyscallContext
  { nscHandler :: SystemCallHandler
  , nscInitial :: !Word64
  , nscConsumed :: IORef Word64
  }

newNativeSyscallContext :: SystemCallHandler -> Word64 -> IO NativeSyscallContext
newNativeSyscallContext h limit = NativeSyscallContext h limit <$> newIORef 0

nscGasRemaining :: NativeSyscallContext -> IO Word64
nscGasRemaining c = do
  used <- readIORef (nscConsumed c)
  pure (if used > nscInitial c then 0 else nscInitial c - used)

nscTotalGasConsumed :: NativeSyscallContext -> IO Word64
nscTotalGasConsumed c = (nscInitial c -) <$> nscGasRemaining c

-- | Saturating; True while gas remains.
nscConsumeGas :: NativeSyscallContext -> Word64 -> IO Bool
nscConsumeGas c amount = do
  used <- readIORef (nscConsumed c)
  let s = used + amount
      used' = if s < used then maxBound else s
  writeIORef (nscConsumed c) used'
  pure (nscInitial c > used')

-- | Run a syscall body with host arguments.  The outputs come back if they
-- were not disarmed.
nativeSyscall :: NativeSyscallContext -> SyscallMeta -> o -> (Word64 -> IO (InnerResult, o)) -> IO (HostFnStatus SyscallErrorCode, Maybe o)
nativeSyscall c meta zero body = do
  remaining <- nscGasRemaining c
  ((res, gas), outs) <- wrapAfterParse meta True True (nscHandler c) remaining zero body
  _ <- nscConsumeGas c gas
  pure (res, outs)

-- | Read a native token balance, billing the access and any first-touch
-- load; refused unless the frame can pay its full cost.
nscNativeTokenBalance :: NativeSyscallContext -> ContractAddress -> IO (SystemStatus (SyscallStatus Integer))
nscNativeTokenBalance c addr = do
  loadGas <- peekNativeTokenBalanceLoadGas (nscHandler c) addr
  let charge = kStorageAccessCost + loadGas
  remaining <- nscGasRemaining c
  if charge > remaining
    then do
      _ <- nscConsumeGas c charge
      pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
    else do
      r <- getNativeTokenBalance (nscHandler c) addr
      case r of
        Left e -> pure (Left e)
        Right (StorageRead res lg) -> do
          _ <- nscConsumeGas c (kStorageAccessCost + lg)
          pure (Right res)

-- | Apply a balance change, charging a flat write cost up front.
nscModifyNativeTokenBalance :: NativeSyscallContext -> ContractAddress -> Integer -> IO (SystemStatus (SyscallStatus ()))
nscModifyNativeTokenBalance c addr amount = do
  remaining <- nscGasRemaining c
  if kStorageAccessCost > remaining
    then do
      _ <- nscConsumeGas c kStorageAccessCost
      pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
    else do
      _ <- nscConsumeGas c kStorageAccessCost
      modifyNativeTokenBalance (nscHandler c) addr amount

type NativeFunction = NativeSyscallContext -> IO (InvokeStatus ())

-- | One native contract: methods keyed by id.
newtype NativeContract = NativeContract (M.Map MethodID NativeFunction)

mkNativeContract :: [(MethodID, NativeFunction)] -> NativeContract
mkNativeContract = NativeContract . M.fromList

-- | NativeContract::invoke: unknown methods fault; a body that used up its
-- whole budget on a deterministic outcome reports OutOfGas.
nativeContractInvoke :: NativeContract -> MethodID -> SystemCallHandler -> Word64 -> IO (InvokeStatus (), Word64)
nativeContractInvoke (NativeContract fns) method h gasLimit = case M.lookup method fns of
  Nothing -> pure (Left Deterministic, 0)
  Just fn -> do
    ctx <- newNativeSyscallContext h gasLimit
    status <- fn ctx
    consumed <- nscTotalGasConsumed ctx
    checkM (consumed <= gasLimit) "native contract exceeded its gas limit"
    let passthrough = case status of
          Left e -> e `elem` [InvokeErrorCode_Unrecoverable, InvokeErrorCode_ValidationFailure, InvokeErrorCode_Abandon, InvokeErrorCode_OutOfGas]
          Right () -> False
    pure $
      if passthrough
        then (status, consumed)
        else
          if gasLimit == consumed
            then (Left InvokeErrorCode_OutOfGas, consumed)
            else (status, consumed)

-- | NativeContext: instantiates native contracts from a fixed set.
nativeContext :: M.Map HogexecPrecompiledNativeContracts NativeContract -> RuntimeContext
nativeContext contracts =
  RuntimeContext
    { rcVerifyContract = \contract _ -> case contract of
        Contract_native_contract_id nid
          | M.member nid contracts -> pure (Right emptyPreparedContract)
          | otherwise -> pure (Left (Deterministic, Just ContractRejectNotDeployable))
        _ -> panic "CHECK failed: native context asked to verify a non-native contract"
    , rcMakeNewRuntime = \dep h -> do
        let dh = dcHash dep
        if dh.contract_type /= KNative
          then do
            logError ("Unrecoverable: Attempt made to access a non-native contract from NativeContext: " ++ show dh.contract_type)
            pure (Left SysUnrecoverable)
          else do
            r <- resolveContract h dh.hash
            case r of
              Left e -> pure (Left e)
              Right Nothing -> do
                logError ("Unrecoverable: Contract lookup failed: " ++ showByteArray dh.hash)
                pure (Left SysUnrecoverable)
              Right (Just (Contract_native_contract_id nid)) -> case M.lookup nid contracts of
                Nothing -> do
                  logWarn ("Attempt made to access a native contract that is not defined: " ++ show nid)
                  pure (Right Nothing)
                Just nc -> pure (Right (Just (Runtime (\m g -> nativeContractInvoke nc m h g))))
              Right (Just other) -> do
                logError ("Unrecoverable: Mismatch in contract type: " ++ show (discriminant_Contract other) ++ " != " ++ show dh.contract_type)
                pure (Left SysUnrecoverable)
    }

-- | Delegates to a context per contract type.
multiplexedContext :: M.Map ContractType RuntimeContext -> RuntimeContext
multiplexedContext ctxs =
  RuntimeContext
    { rcVerifyContract = \contract params -> case M.lookup (discriminant_Contract contract) ctxs of
        Nothing -> pure (Left (Deterministic, Just ContractRejectNotDeployable))
        Just c -> rcVerifyContract c contract params
    , rcMakeNewRuntime = \dep h -> case M.lookup ((dcHash dep).contract_type) ctxs of
        Nothing -> do
          logError ("Unrecoverable: no runtime context registered for contract type " ++ show (dcHash dep).contract_type)
          pure (Left SysUnrecoverable)
        Just c -> rcMakeNewRuntime c dep h
    }
