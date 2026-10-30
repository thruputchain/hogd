-- | The RV32 engine as the executor sees it (runtime/rv32/rv32_contract.cc):
-- contract verification (which is translation), runtime creation, and the
-- ecall loop that connects a running machine to syscall_entry.
module Hogd.Exec.Runtime.Rv32Runtime
  ( rv32Context
  ) where

import Data.Word
import Hogd.Base.NetworkConfigParams (NetworkConfigParams (..))
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Exec.Runtime.Item.StorageSnapshotCache (ContractCacheEntry (..), DeployedContract (..))
import Hogd.Exec.Runtime.PreparedContract
import Hogd.Exec.Runtime.Rv32.Host
import qualified Hogd.Exec.Runtime.Rv32.Machine as M
import Hogd.Exec.Runtime.Rv32.Program (Rv32Limits (..))
import Hogd.Exec.Runtime.Rv32.Rv32Contract
import Hogd.Exec.Runtime.SyscallEntry (GuestMemory (..))
import Hogd.Exec.Runtime.SyscallHandler (resolveContract, resolvePreparedContract)
import Hogd.Exec.Runtime.Syscalls (syscallEntry)
import Hogd.Exec.Runtime.Types
import Hogd.Exec.Util (showByteArray)
import Hogd.Xdr

rv32Context :: RuntimeContext
rv32Context =
  RuntimeContext
    { rcVerifyContract = \contract params -> case contract of
        -- Verify by translating under the chain's bounds, and hand the
        -- translation back for reuse (#3536).
        Contract_rv32_contract blob ->
          pure $ case verifyRv32Blob (Rv32Limits params.max_rv32_code_bytes params.max_rv32_memory_bytes) blob.data_ of
            Left code -> Left (Deterministic, Just code)
            Right c -> Right (PreparedContract (Just c))
        _ -> pure (Left (Deterministic, Just ContractRejectNotDeployable))
    , rcMakeNewRuntime = makeNewRuntime
    }

unrecoverableMsg :: String -> IO (SystemStatus a)
unrecoverableMsg msg = do
  logError ("Unrecoverable: " ++ msg)
  pure (Left SysUnrecoverable)

makeNewRuntime :: DeployedContract -> SystemCallHandler -> IO (SystemStatus (Maybe Runtime))
makeNewRuntime dep h = do
  let dh = dcHash dep
  if dh.contract_type /= KRv32
    then unrecoverableMsg ("Attempt made to access a non-rv32 contract from Rv32Context: " ++ show dh.contract_type)
    else case dcEntry dep of
      -- Snapshot deployment: the cache translated the entry eagerly.
      Just entry -> case pcRv32 (ccePrepared entry) of
        Nothing -> unrecoverableMsg ("rv32 contract is not translatable: " ++ showByteArray dh.hash)
        Just c -> pure (Right (Just (rv32Runtime c h)))
      -- Transaction-local deployment: borrow the image resolved for it.
      Nothing -> do
        cr <- resolveContract h dh.hash
        case cr of
          Left e -> pure (Left e)
          Right Nothing -> unrecoverableMsg ("Contract lookup failed: " ++ showByteArray dh.hash)
          Right (Just contract)
            | discriminant_Contract contract /= dh.contract_type ->
                unrecoverableMsg ("Mismatch in contract type: " ++ show (discriminant_Contract contract) ++ " != " ++ show dh.contract_type)
            | otherwise -> do
                pr <- resolvePreparedContract h dh.hash
                case pr of
                  Left e -> pure (Left e)
                  Right Nothing -> unrecoverableMsg ("Contract lookup failed: " ++ showByteArray dh.hash)
                  Right (Just p) -> case pcRv32 p of
                    Nothing -> unrecoverableMsg ("rv32 contract is not translatable: " ++ showByteArray dh.hash)
                    Just c -> pure (Right (Just (rv32Runtime c h)))

rv32Runtime :: Rv32Contract -> SystemCallHandler -> Runtime
rv32Runtime c h = Runtime $ \methodId gasLimit -> do
  machine <- instantiateContract c methodId gasLimit
  r0 <- M.start machine
  runEcallLoop h machine gasLimit r0

machineMemory :: M.Machine -> GuestMemory
machineMemory m =
  GuestMemory
    { gmSize = M.memorySize m
    , gmRead = \off len -> maybe (error "guest read out of bounds") id <$> M.readGuest m (fromIntegral off) (fromIntegral len)
    , gmWrite = \off bs -> do
        ok <- M.writeGuest m (fromIntegral off) bs
        if ok then pure () else error "guest write out of bounds"
    }

-- | Rv32Runtime::run_ecall_loop.
runEcallLoop :: SystemCallHandler -> M.Machine -> Word64 -> StepResult -> IO (InvokeStatus (), Word64)
runEcallLoop h machine gasLimit = go
  where
    consumed = (gasLimit -) <$> M.gasRemaining machine
    go result = case result of
      StepEcall -> do
        args <- M.pendingEcall machine >>= maybe (panic "ecall without pending arguments") pure
        remaining <- M.gasRemaining machine
        (status, gasUsed) <-
          syscallEntry h (machineMemory machine) remaining args.a0 [args.a1, args.a2, args.a3, args.a4, args.a5, args.a6, args.a7]
        case status of
          Right (SyscallErrorCode code) ->
            M.resumeFromEcall machine (continueWith (fromIntegral code) gasUsed) >>= go
          Left err -> do
            _ <- M.consumeGas machine gasUsed
            total <- consumed
            case err of
              HostFnErrorCode_Unrecoverable -> pure (Left InvokeErrorCode_Unrecoverable, total)
              HostFnErrorCode_ValidationFailure -> pure (Left InvokeErrorCode_ValidationFailure, total)
              HostFnErrorCode_Abandon -> pure (Left InvokeErrorCode_Abandon, total)
              HostFnErrorCode_Return -> pure (Right (), total)
              HostFnErrorCode_OutOfGas -> pure (Left InvokeErrorCode_OutOfGas, total)
              Exit -> pure (Left Deterministic, total)
              _ -> do
                logError "Unrecoverable: HostFnErrorCode::NoneOrRecoverable is not expected from TrampolineResult"
                pure (Left InvokeErrorCode_Unrecoverable, total)
      StepTrap -> do
        t <- M.trap machine
        total <- consumed
        pure (if t == TrapOutOfGas then Left InvokeErrorCode_OutOfGas else Left Deterministic, total)
      _ -> do
        total <- consumed
        logError ("Unrecoverable: unexpected rv32 step result " ++ show result)
        pure (Left InvokeErrorCode_Unrecoverable, total)
