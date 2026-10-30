-- | The per-transaction system call handler (runtime/syscall_handler.{h,cc}).
--
-- Manages the invocation frame stack, storage views, nondeterminism logging
-- and the two-phase commit of one transaction.
module Hogd.Exec.Runtime.SyscallHandler
  ( -- * Construction
    newSystemCallHandler
  , newSystemCallHandlerTxless
    -- * Driving a transaction
  , rootInvoke
  , finalizeAfterRootFrame
  , invoke
  , validateNewInvocation
  , startFrame
  , finishFrame
  , tryCommitTransaction
  , abortTransaction
  , emitEvents
  , extractProposalData
  , getMostRecentFrameOutput
  , getMostRecentFrameEvents
  , rootInvokeResult
  , lastFrameFinished
    -- * Error conversions
  , invokeToExecResult
  , systemToExecResult
    -- * Frame queries
  , getCurrentAddress
  , getMsgSender
  , getSourceTxHash
  , getCalldata
  , getParentSuppliedAuthTokenCount
  , getAuthClaim
  , getReturnBuffer
  , getReturnedAuthTokens
  , setReturnBuffer
  , isReadOnly
  , isRunning
  , checkAuthTokensOwned
  , checkParentCanTakeAuthTokens
    -- * Events and deployment
  , logEvent
  , staticGetDeployAddress
  , deployContract
    -- * Crypto streams
  , sha256Update
  , sha256Copy
  , sha256Finalize
    -- * Storage
  , StorageRead (..)
  , getBytestring
  , setBytestring
  , getNnint
  , setNnint
  , deleteObject
  , setStorageFlags
  , getStorageFlags
  , clearStorageFlags
  , retrocausalQueueInsert
  , retrocausalQueueConsume
    -- * Native token
  , peekNativeTokenBalanceLoadGas
  , getNativeTokenBalance
  , modifyNativeTokenBalance
    -- * Authentication
  , changePublicKeyRegistration
  , setAddressRequiredWeight
  , mintAuthToken
  , consumeAuthToken
    -- * Block data
  , getBlockHeight
  , getNetworkHash
  , getBlockTimepoint
    -- * Contracts
  , resolveContract
  , resolvePreparedContract
  , configParams
    -- * Method ids
  , sdkMethodId
  , entrypointFromMethodId
  ) where

import Control.Monad (forM_, unless, when)
import qualified Crypto.Hash as H
import Data.Bits ((.&.), (.|.))
import qualified Data.ByteArray as BA
import qualified Data.ByteString as B
import Data.Foldable (toList)
import Data.IORef
import qualified Data.Map.Strict as M
import qualified Data.Set as S
import Data.Word
import Hogd.Base.NetworkConfigParams (NetworkConfigParams (..))
import Hogd.Exec.Common.Int128
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ExecutionMode
import Hogd.Exec.Log
import qualified Hogd.Exec.Metrics as Metrics
import Hogd.Exec.Runtime.EventServerProxy (PerTransactionEventServerProxy, ptxEmitEvents, ptxReportTxFailure)
import Hogd.Exec.Runtime.GasCostTable (kDBRPCCost)
import Hogd.Exec.Runtime.InvocationFrame
import Hogd.Exec.Runtime.Item.DatabaseProxy (PerTransactionDatabaseProxy, ptxAbortTransaction, ptxAddDelta, ptxCommitTransaction, ptxLastPushConflict, ptxTryPushCurrentSubtransaction)
import Hogd.Exec.Runtime.Item.ItemView
import Hogd.Exec.Runtime.Item.SnapshotStorageItem
import Hogd.Exec.Runtime.Item.StorageSnapshotCache
import Hogd.Exec.Runtime.PreparedContract
import Hogd.Exec.Runtime.Types
import Hogd.Exec.Util (showByteArray)
import Hogd.Xdr
import X23.Xdr (xdrEncodeStrict)

------------------------------------------------------------------------------
-- Helpers

sha256 :: B.ByteString -> B.ByteString
sha256 = BA.convert . H.hashWith H.SHA256

sha256Xdr :: XdrHashable a => a -> B.ByteString
sha256Xdr = sha256 . encodeForHash

class XdrHashable a where
  encodeForHash :: a -> B.ByteString

instance XdrHashable Transaction where
  encodeForHash = xdrEncodeStrict

instance XdrHashable ContractDeploymentHashPreimage where
  encodeForHash = xdrEncodeStrict

instance XdrHashable Contract where
  encodeForHash = xdrEncodeStrict

-- | The 8-byte entrypoint read as a big-endian integer.
sdkMethodId :: ContractEntrypointID -> MethodID
sdkMethodId = B.foldl' (\acc b -> acc * 256 + fromIntegral b) 0

entrypointFromMethodId :: MethodID -> ContractEntrypointID
entrypointFromMethodId m = B.pack [fromIntegral (m `div` (256 ^ (7 - i))) | i <- [0 .. 7 :: Int]]

contractBlobSize :: Contract -> Int
contractBlobSize = \case
  Contract_rv32_contract b -> B.length b.data_
  _ -> 0

contractTypeOf :: Contract -> ContractType
contractTypeOf = discriminant_Contract

flagBit :: InvokeFlag -> Word32
flagBit (InvokeFlag f) = fromIntegral f

-- | Only TryInvoke and ReadOnly are defined invoke flags.
validInvokeFlags :: InvokeFlagSet -> Bool
validInvokeFlags flags = flags .&. (flagBit TryInvoke .|. flagBit ReadOnly) == flags

blockExecutionModeOf :: SystemCallHandler -> BlockExecutionMode
blockExecutionModeOf h = case schMode h of
  ModeProposal _ -> KBlockExecutionModeProposal
  ModeValidation _ -> KBlockExecutionModeValidation
  ModeViewing _ -> KBlockExecutionModeViewing

------------------------------------------------------------------------------
-- Construction

baseHandler
  :: RuntimeContext
  -> StorageSnapshotCache
  -> PerTransactionDatabaseProxy
  -> PerTransactionEventServerProxy
  -> ExecutionModeCtor
  -> AuthClaim
  -> SHA256Hash
  -> Maybe Invocation
  -> Maybe Uniquifier
  -> [Contract]
  -> IO SystemCallHandler
baseHandler rc cache db ev ctor claim txHash rootInv uniq contracts = do
  params <- getNetworkConfigParams cache
  mode <- newExecutionModeData ctor
  SystemCallHandler rc db ev
    <$> newIORef []
    <*> pure params
    <*> pure txHash
    <*> pure claim
    <*> newIORef rootInv
    <*> pure uniq
    <*> pure (fmap (\i -> i.invoked_address) rootInv)
    <*> pure mode
    <*> pure contracts
    <*> newIORef M.empty
    <*> newIORef Nothing
    <*> newIORef (InvokeResult (Right ()) 0)
    <*> newIORef False
    <*> newIORef False
    <*> newIORef False
    <*> newIORef False
    <*> newIORef False
    <*> pure cache
    <*> newIORef S.empty

-- | A handler for a real transaction (proposal or validation mode only).
newSystemCallHandler
  :: RuntimeContext
  -> StorageSnapshotCache
  -> PerTransactionDatabaseProxy
  -> PerTransactionEventServerProxy
  -> ExecutionModeCtor
  -> Transaction
  -> IO SystemCallHandler
newSystemCallHandler rc cache db ev ctor tx = do
  case ctor of
    ViewingModeCtor _ -> panic "CHECK failed: viewing mode handler built from a transaction"
    _ -> pure ()
  baseHandler rc cache db ev ctor tx.auth_claim (sha256Xdr tx) (Just tx.invocation) (Just tx.uniquifier) tx.contracts_to_deploy

-- | The transaction-less handler (views and test harnesses): no root
-- invocation, uniquifier or attachments.
newSystemCallHandlerTxless
  :: RuntimeContext
  -> StorageSnapshotCache
  -> PerTransactionDatabaseProxy
  -> PerTransactionEventServerProxy
  -> ExecutionModeCtor
  -> AuthClaim
  -> SHA256Hash
  -> IO SystemCallHandler
newSystemCallHandlerTxless rc cache db ev ctor claim txHash =
  baseHandler rc cache db ev ctor claim txHash Nothing Nothing []

configParams :: SystemCallHandler -> NetworkConfigParams
configParams = schConfig

------------------------------------------------------------------------------
-- Error conversions

invokeToExecResult :: InvokeErrorCode -> IO ExecResult
invokeToExecResult = \case
  InvokeErrorCode_None -> pure ExecResultOK
  InvokeErrorCode_Abandon -> pure ExecResultSystemError
  Deterministic -> pure ExecResultTxDeterministicError
  InvokeErrorCode_OutOfGas -> pure ExecResultTxDeterministicError
  InvokeErrorCode_ValidationFailure -> pure ExecResultTxValidationFailure
  InvokeErrorCode_Unrecoverable -> do
    logError "make_exec_result: received Unrecoverable"
    pure ExecResultUnrecoverable
  _ -> pure ExecResultUnrecoverable

systemToExecResult :: SystemError -> ExecResult
systemToExecResult = \case
  SystemErrorCode_Abandon -> ExecResultSystemError
  SystemErrorCode_ValidationFailure -> ExecResultTxValidationFailure
  _ -> ExecResultUnrecoverable

------------------------------------------------------------------------------
-- Root invocation

-- | The executing block height (the pinned parent tip's plus one).
getBlockHeight :: SystemCallHandler -> IO BlockHeight
getBlockHeight h = do
  tip <- getDbTip (schSnapshot h)
  pure (tip.value.proposal_header.block_height + 1)

getNetworkHash :: SystemCallHandler -> IO NetworkHash
getNetworkHash h = do
  tip <- getDbTip (schSnapshot h)
  pure tip.value.proposal_header.network_hash

-- | From the BlockExecutionContext the block was opened with.
getBlockTimepoint :: SystemCallHandler -> IO TimePoint
getBlockTimepoint h = (\c -> c.block_timepoint) <$> blockExecutionContext (schSnapshot h)

-- | Whether this transaction's replay uniquifier already expired at the
-- executing height (the replay cache rejects block_expiry_height < height).
replayUniquifierExpired :: SystemCallHandler -> IO Bool
replayUniquifierExpired h = case schUniquifier h of
  Nothing -> pure False
  Just u -> (u.block_expiry_height <) <$> getBlockHeight h

reportTxFailure :: SystemCallHandler -> TxFailureCause -> IO ()
reportTxFailure h cause = do
  height <- getBlockHeight h
  ptxReportTxFailure (schEvents h) (schSourceTxHash h) TxFailureDiagnostic {attempt_height = height, cause = cause}

reportContractRejection :: SystemCallHandler -> ContractRejectDiagnostic -> IO ()
reportContractRejection h rej = when (isProposal (schMode h)) $ reportTxFailure h (TxFailureCause_contract rej)

-- | On a failed 2pc push, best-effort report the first conflict diagnostic.
reportDbConflictDiagnostic :: SystemCallHandler -> IO ()
reportDbConflictDiagnostic h = do
  conflict <- ptxLastPushConflict (schDb h)
  case conflict of
    DbConflictResult_kDbDiagnosticNone -> pure ()
    DbConflictResult_first d -> send conflict d
    DbConflictResult_conflicts (d : _) -> send conflict d
    DbConflictResult_conflicts [] -> panic "CHECK failed: kDbDiagnosticAll with no conflicts"
  where
    send conflict d = do
      logTrace ("two-phase commit conflict diagnostics: " ++ show conflict)
      reportTxFailure h (TxFailureCause_conflict d)

-- | Validate that the transaction can be run: unexpired uniquifier,
-- well-formed proposal data, budget and attachment limits, and attachments
-- that verify.
performInitialValidation :: SystemCallHandler -> IO (InvokeStatus (), Word64)
performInitialValidation h = do
  expired <- replayUniquifierExpired h
  let params = schConfig h
      contracts = schContractsToDeploy h
      txHashStr = showByteArray (schSourceTxHash h)
  if expired
    then pure (Left InvokeErrorCode_ValidationFailure, 0)
    else do
      let proposalOk = case schMode h of
            ModeValidation v -> validateProposalData v
            _ -> True
      rootInv <- readIORef (schRootInvocation h)
      let overBudget = maybe False (\i -> i.gas_limit > params.max_tx_gas) rootInv
      if not proposalOk
        then pure (Left InvokeErrorCode_ValidationFailure, 0)
        else
          if overBudget
            then do
              logWarn ("tx " ++ txHashStr ++ ": budgets " ++ show (maybe 0 (\i -> i.gas_limit) rootInv) ++ " gas; chain permits " ++ show params.max_tx_gas)
              pure (Left InvokeErrorCode_ValidationFailure, 0)
            else
              if length contracts > fromIntegral params.max_new_contracts_per_tx
                then do
                  logWarn ("tx " ++ txHashStr ++ ": attaches " ++ show (length contracts) ++ " contracts; chain permits " ++ show params.max_new_contracts_per_tx)
                  reportContractRejection
                    h
                    ContractRejectDiagnostic {contract_index = params.max_new_contracts_per_tx, code = ContractRejectTooMany}
                  pure (Left InvokeErrorCode_ValidationFailure, 0)
                else case [i | (i, c) <- zip [0 :: Int ..] contracts, contractBlobSize c > fromIntegral params.max_contract_size_bytes] of
                  (i : _) -> do
                    logWarn
                      ( "tx " ++ txHashStr ++ ": attached contract " ++ show i ++ " is "
                          ++ show (contractBlobSize (contracts !! i)) ++ " bytes; chain permits " ++ show params.max_contract_size_bytes
                      )
                    reportContractRejection h ContractRejectDiagnostic {contract_index = fromIntegral i, code = ContractRejectBlobTooLarge}
                    pure (Left InvokeErrorCode_ValidationFailure, 0)
                  [] -> makeNewContractSet h

-- | NewContractSet::make: structural gates first, then verify (translate)
-- each attachment, keeping the prepared image.
makeNewContractSet :: SystemCallHandler -> IO (InvokeStatus (), Word64)
makeNewContractSet h = do
  let contracts = zip [0 :: Int ..] (schContractsToDeploy h)
      reject i code = do
        let rej = ContractRejectDiagnostic {contract_index = fromIntegral i, code = code}
        logWarn ("tx " ++ showByteArray (schSourceTxHash h) ++ ": refusing attached contract " ++ show i ++ ": " ++ show code)
        reportContractRejection h rej
        pure (Left InvokeErrorCode_ValidationFailure, 0)
      structural seen [] = Right (reverse seen)
      structural seen ((i, c) : rest)
        | contractTypeOf c /= KRv32 = Left (i, ContractRejectNotDeployable)
        | otherwise =
            let hsh = sha256Xdr c
             in if any (\(_, hh, _) -> hh == hsh) seen
                  then Left (i, ContractRejectDuplicate)
                  else structural ((i, hsh, c) : seen) rest
  case structural [] contracts of
    Left (i, code) -> reject i code
    Right hashed -> do
      let verify acc [] = pure (Right acc)
          verify acc ((i, hsh, c) : rest) = do
            r <- rcVerifyContract (schRuntimeContext h) c (schConfig h)
            case r of
              Left (Deterministic, reason) -> case reason of
                Just code -> pure (Left (Just (i, code), Deterministic))
                Nothing -> panic "CHECK failed: Deterministic rejection without a reason"
              Left (code, _) -> pure (Left (Nothing, code))
              Right prepared -> do
                checkM (maybe False (const True) (pcRv32 prepared)) "rv32 verification produced no image"
                verify (M.insert hsh (c, prepared) acc) rest
      r <- verify M.empty hashed
      case r of
        Left (Just (i, code), _) -> reject i code
        Left (Nothing, code) -> pure (Left code, 0)
        Right m -> do
          writeIORef (schNewContracts h) m
          pure (Right (), 0)

checkFinalValidationData :: SystemCallHandler -> IO Bool
checkFinalValidationData h = case schMode h of
  ModeValidation v -> checkAllConsumed v
  _ -> pure True

-- | Root invocation of the transaction.  Must be called once.
rootInvoke :: SystemCallHandler -> IO InvokeResult
rootInvoke h = do
  (initRes, initGas) <- performInitialValidation h
  case initRes of
    Left e -> do
      writeIORef (schInitialValidationRejected h) True
      finalizeAfterRootFrame h (InvokeResult (Left e) initGas)
    Right () -> do
      mroot <- readIORef (schRootInvocation h)
      root <- maybe (panic "CHECK failed: root_invocation_ missing") pure mroot
      writeIORef (schRootInvocation h) Nothing
      let available = if initGas < root.gas_limit then root.gas_limit - initGas else 0
      admitted <- validateNewInvocation h root.invoked_address 0 []
      case admitted of
        Left _ -> finalizeAfterRootFrame h (InvokeResult (Left Deterministic) initGas)
        Right () -> do
          (res, gas) <- invoke h root.invoked_address (sdkMethodId root.entrypoint) available root.calldata 0 []
          status <- handleRootHostFnStatus res
          finalizeAfterRootFrame h (InvokeResult status (gas + initGas))

-- | Store the root result, rewriting it to ValidationFailure if final
-- validation fails; marks the root invocation finished.
finalizeAfterRootFrame :: SystemCallHandler -> InvokeResult -> IO InvokeResult
finalizeAfterRootFrame h result = do
  writeIORef (schFinalized h) True
  writeIORef (schLastFrameFinished h) True
  ok <- checkFinalValidationData h
  (fs, _) <- toFrameStatus result
  let result' = case (ok, fs) of
        (False, Right _) -> result {irResult = Left InvokeErrorCode_ValidationFailure}
        _ -> result
  writeIORef (schRootResult h) result'
  pure result'

rootInvokeResult :: SystemCallHandler -> IO InvokeResult
rootInvokeResult h = readIORef (schRootResult h)

lastFrameFinished :: SystemCallHandler -> IO Bool
lastFrameFinished h = readIORef (schLastFrameFinished h)

rootInvokeSucceeded :: SystemCallHandler -> IO Bool
rootInvokeSucceeded h = do
  fin <- readIORef (schFinalized h)
  r <- readIORef (schRootResult h)
  pure (fin && isRootSuccess r)

handleRootHostFnStatus :: SystemStatus (SyscallStatus InvocationFrameStatusCode) -> IO (InvokeStatus ())
handleRootHostFnStatus = \case
  Right (Left _) -> pure (Left Deterministic)
  Right (Right st) -> case st of
    InvocationFrameStatusCode_Return -> pure (Right ())
    Abort -> pure (Right ())
    Fault -> pure (Left Deterministic)
    _ -> do
      logError ("handle_root_host_fn_status: received unknown frame status " ++ show st)
      pure (Left InvokeErrorCode_Unrecoverable)
  Left e -> case e of
    SystemErrorCode_ValidationFailure -> pure (Left InvokeErrorCode_ValidationFailure)
    SystemErrorCode_Abandon -> pure (Left InvokeErrorCode_Abandon)
    _ -> do
      logError "handle_root_host_fn_status: received Unrecoverable"
      pure (Left InvokeErrorCode_Unrecoverable)

------------------------------------------------------------------------------
-- Invocation

isRunning :: SystemCallHandler -> IO Bool
isRunning h = not <$> stackEmpty h

isReadOnly :: SystemCallHandler -> IO Bool
isReadOnly h = frameIsReadOnlyIO =<< activeFrame h
  where
    frameIsReadOnlyIO = pure . frameIsReadOnly

-- | Admission checks a new frame must pass, in precedence order: valid
-- flags, no reentrance, room under the depth bound, owned auth tokens.
validateNewInvocation :: SystemCallHandler -> ContractAddress -> InvokeFlagSet -> [Word32] -> IO (SyscallStatus ())
validateNewInvocation h addr flags tokens
  | not (validInvokeFlags flags) = pure (Left KSyscallErrorCodeInvalidSyscallInput)
  | otherwise = do
      reentrant <- hasActiveFrameFor h addr
      depth <- stackSize h
      empty <- stackEmpty h
      if reentrant
        then pure (Left KSyscallErrorCodeReentrance)
        else
          if depth >= fromIntegral (schConfig h).max_cross_invoke_depth
            then pure (Left KSyscallErrorCodeInvokeDepthExceeded)
            else
              if empty
                then pure (Right ())
                else do
                  owned <- checkAuthTokensOwned h tokens
                  pure (if owned then Right () else Left KSyscallErrorCodeInvalidSyscallInput)

-- | Runtime::invoke: an out-of-gas result charges the whole limit.
runRuntime :: Runtime -> MethodID -> Word64 -> IO InvokeResult
runRuntime rt method gasLimit = do
  logTrace ("Invoking method " ++ show method ++ " with gas limit " ++ show gasLimit)
  (res, gas0) <- rtInvokeInternal rt method gasLimit
  logTrace ("Invoke result: " ++ either show (const "success") res ++ ", gas consumed: " ++ show gas0)
  let gas = case res of
        Left InvokeErrorCode_OutOfGas -> gasLimit
        _ -> gas0
  checkM (gas <= gasLimit) "runtime consumed more than its gas limit"
  pure (InvokeResult res gas)

-- | Create and run a child frame (callers ran 'validateNewInvocation').
-- Outer Left: block-ending error.  Inner Left: rejected before any frame
-- ran.  Right: how the child frame ended.
invoke
  :: SystemCallHandler
  -> ContractAddress
  -> MethodID
  -> Word64
  -> Calldata
  -> InvokeFlagSet
  -> [Word32]
  -> IO (SystemStatus (SyscallStatus InvocationFrameStatusCode), Word64)
invoke h addr method gasLimit calldata flags0 tokens = do
  checkM (validInvokeFlags flags0) "invoke with invalid flags"
  empty <- stackEmpty h
  ro <- if empty then pure False else isReadOnly h
  let flags = if ro then flags0 .|. flagBit ReadOnly else flags0
  runtime <- resolveRuntime h addr
  case runtime of
    Left e -> do
      logTrace ("SystemError: resolving runtime for address: " ++ showByteArray addr ++ ", error " ++ show e)
      pure (Left e, 0)
    Right Nothing -> do
      logTrace ("Failed to instantiate runtime for address: " ++ showByteArray addr)
      pure (Right (Left KSyscallErrorCodeInvalidSyscallInput), 0)
    Right (Just rt) -> do
      startFrame h addr calldata flags tokens
      cumulative <- runRuntime rt method gasLimit
      finished <- finishFrame h (irResult cumulative)
      case finished of
        Left e -> pure (Left e, 0)
        Right (frameResult, manuallyReverted) -> do
          let total = addInvokeResult cumulative frameResult
          (fs, gas) <- toFrameStatus total
          case fs of
            Left e -> pure (Left e, gas)
            Right st ->
              let st' = if st == InvocationFrameStatusCode_Return && manuallyReverted then Abort else st
               in pure (Right (Right st'), gas)

startFrame :: SystemCallHandler -> ContractAddress -> Calldata -> InvokeFlagSet -> [Word32] -> IO ()
startFrame h addr calldata flags tokens = do
  done <- readIORef (schLastFrameFinished h)
  checkM (not done) "start_frame after the root frame finished"
  startFrameStack h addr calldata flags tokens

-- | SystemCallHandler::finish_frame.
finishFrame :: SystemCallHandler -> InvokeStatus () -> IO (SystemStatus (InvokeResult, Bool))
finishFrame h result = do
  case result of
    Left _ -> setActiveFrameReturn h InvocationFrameOutput_Fault []
    Right () -> pure ()
  (r, _conflict) <- finishFrameStack h result (\f -> writeIORef (schLastFrame h) (Just f))
  empty <- stackEmpty h
  when empty $ writeIORef (schLastFrameFinished h) True
  pure r

------------------------------------------------------------------------------
-- Frame outputs

getMostRecentFrameOutput :: SystemCallHandler -> IO InvocationFrameOutput
getMostRecentFrameOutput h = do
  done <- readIORef (schLastFrameFinished h)
  if done
    then maybe InvocationFrameOutput_Fault ifToReturn <$> readIORef (schLastFrame h)
    else do
      frames <- readIORef (schFrames h)
      case frames of
        [] -> pure InvocationFrameOutput_Fault
        (f : _) -> maybe (panic "no child return output") pure (ifChildReturnOutput f)

getMostRecentFrameEvents :: SystemCallHandler -> IO [TxEmittedEvent]
getMostRecentFrameEvents h = do
  done <- readIORef (schLastFrameFinished h)
  if done
    then do
      lf <- readIORef (schLastFrame h)
      pure $ case lf of
        Just f | not (frameShouldRevert f) -> toList (ifEvents f)
        _ -> []
    else do
      frames <- readIORef (schFrames h)
      pure $ case frames of
        [] -> []
        (f : _) -> toList (ifEvents f)

------------------------------------------------------------------------------
-- Commit

abortAfterCommitFailure :: SystemCallHandler -> ExecResult -> Bool -> IO ExecResult
abortAfterCommitFailure h failure reportDiagnostic = do
  writeIORef (schTwoPhaseCommitFailed h) True
  when reportDiagnostic $ reportDbConflictDiagnostic h
  r <- abortTransaction h
  pure (either systemToExecResult (const failure) r)

-- | Commit the transaction, or abort it on any failure.  Requires
-- 'rootInvoke' first.
tryCommitTransaction :: SystemCallHandler -> IO ExecResult
tryCommitTransaction h = do
  root <- readIORef (schRootResult h)
  case irResult root of
    Left code -> do
      when (code == InvokeErrorCode_Unrecoverable) $ logError "try_commit_transaction: received Unrecoverable"
      -- Deterministic faults are included: body discarded, side records kept.
      if code == Deterministic || code == InvokeErrorCode_OutOfGas
        then invokeToExecResult code >>= commitBodyDiscarded h
        else do
          r <- abortTransaction h
          case r of
            Left e -> pure (systemToExecResult e)
            Right () -> invokeToExecResult code
    Right () -> do
      mlast <- readIORef (schLastFrame h)
      lastFrame <- maybe (panic "root succeeded without a last frame") pure mlast
      if frameShouldRevert lastFrame
        then do
          -- Discard the reverted transaction's events.
          writeIORef (schLastFrame h) (Just lastFrame {ifEvents = mempty})
          commitBodyDiscarded h ExecResultTxRootFrameReverted
        else do
          observations <- observationsToValidate (schMode h)
          addReplayNonceDelta h
          -- try_acquire_frame_locks works on the active frame; the root frame
          -- has been popped, so push it back for the duration of the call.
          writeIORef (schFrames h) [lastFrame]
          r <- tryAcquireFrameLocks h observations
          writeIORef (schFrames h) []
          case r of
            Left e -> abortAfterCommitFailure h (systemToExecResult e) True
            Right False -> abortAfterCommitFailure h ExecResultTxNonDeterministicError True
            Right True -> do
              newContracts <- readIORef (schNewContracts h)
              let wanted = frameCommittedContracts lastFrame
                  batch = [c | (hsh, (c, _)) <- M.toList newContracts, hsh `S.member` wanted]
              c <- ptxCommitTransaction (schDb h) batch
              case c of
                Left e -> abortAfterCommitFailure h (systemToExecResult e) False
                Right () -> do
                  Metrics.markTxSuccess root (blockExecutionModeOf h)
                  writeIORef (schCommittedOrAborted h) True
                  pure ExecResultOK

abortTransaction :: SystemCallHandler -> IO (SystemStatus ())
abortTransaction h = do
  writeIORef (schCommittedOrAborted h) True
  modifyIORef' (schLastFrame h) (fmap (\f -> f {ifEvents = mempty}))
  root <- readIORef (schRootResult h)
  Metrics.markTxFailed root (blockExecutionModeOf h)
  ptxAbortTransaction (schDb h)

-- | The replay-nonce consume delta for the root invoked address.
addReplayNonceDelta :: SystemCallHandler -> IO ()
addReplayNonceDelta h = case (schUniquifier h, schRootInvokedAddress h) of
  (Just u, Just addr) ->
    ptxAddDelta
      (schDb h)
      AddressAndKey {address = addr, key = StorageKey_ReplayCache}
      (StorageDelta_consume_uniquifier_args ConsumeUniquifierArgs {uniquifier = u})
      0
  _ -> pure ()

-- | Commit a transaction whose body effects are discarded, keeping its
-- observation deltas (validation) and replay nonce.
commitBodyDiscarded :: SystemCallHandler -> ExecResult -> IO ExecResult
commitBodyDiscarded h success = do
  case schMode h of
    ModeValidation _ -> do
      obs <- observationsToValidate (schMode h)
      forM_ (M.toList obs) $ \(ak, es) ->
        unless (null es) $
          ptxAddDelta
            (schDb h)
            ak
            ( StorageDelta_retrocausal_queue
                RetrocausalQueueDelta
                  { consume = Nothing
                  , insert = RetrocausalQueueInsertDelta {added_entries = []}
                  , observed_concurrent_entries = es
                  }
            )
            0
    _ -> pure ()
  addReplayNonceDelta h
  pushed <- ptxTryPushCurrentSubtransaction (schDb h)
  case pushed of
    Left e -> abortAfterCommitFailure h (systemToExecResult e) False
    Right False -> abortAfterCommitFailure h ExecResultTxNonDeterministicError True
    Right True -> do
      c <- ptxCommitTransaction (schDb h) []
      case c of
        Left e -> abortAfterCommitFailure h (systemToExecResult e) False
        Right () -> do
          root <- readIORef (schRootResult h)
          Metrics.markTxFailed root (blockExecutionModeOf h)
          writeIORef (schCommittedOrAborted h) True
          pure success

-- | Emit the transaction's event log to the event server.
emitEvents :: SystemCallHandler -> ExecResult -> ProposedTransaction -> IO (SystemStatus ())
emitEvents h execResult ptx = do
  ok <- rootInvokeSucceeded h
  lf <- readIORef (schLastFrame h)
  root <- readIORef (schRootResult h)
  let events = case lf of
        Just f | ok -> toList (ifEvents f)
        _ -> []
      output = maybe InvocationFrameOutput_Fault ifToReturn lf
  ptxEmitEvents
    (schEvents h)
    TxEventLog
      { tx_hash = schSourceTxHash h
      , exec_result = execResult
      , root_output = output
      , gas_consumed = irGasConsumed root
      , events = events
      }
    ptx

-- | Proposal mode only.
extractProposalData :: SystemCallHandler -> IO NondeterministicProposalData
extractProposalData h = case schMode h of
  ModeProposal pd -> proposalData pd
  _ -> panic "extract_proposal_data outside proposal mode"

------------------------------------------------------------------------------
-- Frame queries

getCurrentAddress :: SystemCallHandler -> IO ContractAddress
getCurrentAddress h = ifAddress <$> activeFrame h

getMsgSender :: SystemCallHandler -> IO (SyscallStatus ContractAddress)
getMsgSender h = maybe (Left KSyscallErrorCodeIsRootTransaction) Right <$> getMsgSenderStack h

getSourceTxHash :: SystemCallHandler -> SHA256Hash
getSourceTxHash = schSourceTxHash

getCalldata :: SystemCallHandler -> IO Calldata
getCalldata h = ifCalldata <$> activeFrame h

getParentSuppliedAuthTokenCount :: SystemCallHandler -> IO Word32
getParentSuppliedAuthTokenCount h = ifForwardedCount <$> activeFrame h

getAuthClaim :: SystemCallHandler -> AuthClaim
getAuthClaim = schAuthClaim

-- | The buffer the most recent child returned (or aborted with).
getReturnBuffer :: SystemCallHandler -> IO (SystemStatus (SyscallStatus ReturnBuffer))
getReturnBuffer h = do
  f <- activeFrame h
  pure $ case ifChildReturnOutput f of
    Nothing -> Right (Left KSyscallErrorCodeNoReturnBufferAvailable)
    Just (InvocationFrameOutput_return_buffer b) -> Right (Right b)
    Just (InvocationFrameOutput_abort_buffer b) -> Right (Right b)
    Just InvocationFrameOutput_Fault -> Right (Left KSyscallErrorCodeNoReturnBufferAvailable)

getReturnedAuthTokens :: SystemCallHandler -> IO (Word32, Word32)
getReturnedAuthTokens h = ifChildReturnedTokens <$> activeFrame h

setReturnBuffer :: SystemCallHandler -> ReturnBuffer -> Bool -> [Word32] -> IO ()
setReturnBuffer h buf revert tokens
  | revert = setActiveFrameReturn h (InvocationFrameOutput_abort_buffer buf) []
  | otherwise = setActiveFrameReturn h (InvocationFrameOutput_return_buffer buf) tokens

------------------------------------------------------------------------------
-- Events and deployment

-- | Log an event built from up to three topics and a payload.
logEvent :: SystemCallHandler -> [Topic] -> B.ByteString -> IO (SyscallStatus ())
logEvent h topics payload = do
  ro <- isReadOnly h
  checkM (not ro) "log_event in a read-only frame"
  f <- activeFrame h
  let size = fromIntegral (B.length payload) + sum (map (fromIntegral . B.length) topics) :: Integer
  if size > kMaxEventLength
    then pure (Left KSyscallErrorCodeInvalidSyscallInput)
    else
      if ifEventCount f >= kMaxEventsPerTransaction
        then pure (Left KSyscallErrorCodeInvalidSyscallInput)
        else do
          let ev =
                TxEmittedEvent
                  { contract_address = ifAddress f
                  , num_topics = fromIntegral (length topics)
                  , serialized_event = B.concat topics <> payload
                  }
          modifyActiveFrame h (frameLogEvent ev)
          pure (Right ())

staticGetDeployAddress :: ContractAddress -> SHA256Hash -> SHA256Hash -> ContractAddress
staticGetDeployAddress sender nonce chash =
  sha256Xdr ContractDeploymentHashPreimage {deployer_address = sender, nonce = nonce, contract_hash = chash}

deployContract :: SystemCallHandler -> SHA256Hash -> SHA256Hash -> IO (SystemStatus (SyscallStatus ContractAddress))
deployContract h nonce chash = do
  ro <- isReadOnly h
  checkM (not ro) "deploy_contract in a read-only frame"
  cr <- resolveContract h chash
  case cr of
    Left e -> pure (Left e)
    Right Nothing -> pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
    Right (Just contract) -> case contract of
      -- Precompiles are set up at genesis and may not be redeployed.
      Contract_native_contract_id nid
        | nid == KNativeContractNativeToken || nid == KNativeContractDefaultWallet ->
            pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
        | otherwise -> panic ("Unknown native contract deployed: " ++ show nid)
      _ -> do
        sender <- getCurrentAddress h
        let addr = staticGetDeployAddress sender nonce chash
        existing <- tryGetDeployedContract (schSnapshot h) addr
        case existing of
          Left e -> pure (Left e)
          Right (Just _) -> pure (Right (Left KSyscallErrorCodeContractAlreadyDeployed))
          Right Nothing -> do
            local <- lookupLocallyDeployedContract h addr
            case local of
              Just _ -> pure (Right (Left KSyscallErrorCodeContractAlreadyDeployed))
              Nothing -> do
                let dep =
                      DeployedContractHash
                        { hash = chash
                        , contract_size = fromIntegral (contractBlobSize contract)
                        , contract_type = contractTypeOf contract
                        }
                f <- activeFrame h
                f' <- frameDeployContract addr dep f
                modifyActiveFrame h (const f')
                pure (Right (Right addr))

------------------------------------------------------------------------------
-- Hash streams

modifyHashBuffers :: SystemCallHandler -> (HashBuffers -> (a, HashBuffers)) -> IO a
modifyHashBuffers h g = do
  f <- activeFrame h
  let (a, hb') = g (ifHashBuffers f)
  modifyActiveFrame h (\fr -> fr {ifHashBuffers = hb'})
  pure a

sha256Update :: SystemCallHandler -> Word32 -> B.ByteString -> Bool -> IO (SyscallStatus ())
sha256Update h idx dat reset = modifyHashBuffers h $ \hb ->
  let streams = hbStreams hb
   in if not reset
        then case M.lookup idx streams of
          Nothing -> (Left KSyscallErrorCodeInvalidSyscallInput, hb)
          Just ctx -> (Right (), hb {hbStreams = M.insert idx (H.hashUpdate ctx dat) streams})
        else
          if M.size streams < fromIntegral (hbMax hb) || M.member idx streams
            then (Right (), hb {hbStreams = M.insert idx (H.hashUpdate H.hashInit dat) streams})
            else (Left KSyscallErrorCodeInvalidSyscallInput, hb)

sha256Copy :: SystemCallHandler -> Word32 -> Word32 -> IO (SyscallStatus ())
sha256Copy h src dst = modifyHashBuffers h $ \hb ->
  let streams = hbStreams hb
   in case M.lookup src streams of
        Nothing -> (Left KSyscallErrorCodeInvalidSyscallInput, hb)
        Just ctx
          | src == dst -> (Right (), hb)
          | M.size streams < fromIntegral (hbMax hb) || M.member dst streams ->
              (Right (), hb {hbStreams = M.insert dst ctx streams})
          | otherwise -> (Left KSyscallErrorCodeInvalidSyscallInput, hb)

sha256Finalize :: SystemCallHandler -> Word32 -> IO (SyscallStatus SHA256Hash)
sha256Finalize h idx = modifyHashBuffers h $ \hb ->
  case M.lookup idx (hbStreams hb) of
    Nothing -> (Left KSyscallErrorCodeInvalidSyscallInput, hb)
    Just ctx -> (Right (BA.convert (H.hashFinalize ctx)), hb {hbStreams = M.delete idx (hbStreams hb)})

------------------------------------------------------------------------------
-- Storage

-- | What a storage read found and what its database load cost (nonzero only
-- on the transaction's first touch of the key).
data StorageRead a = StorageRead
  { srResult :: SyscallStatus a
  , srLoadGas :: Word64
  }

-- | Record a load; kDBRPCCost the first time, nothing after.
noteItemLoad :: SystemCallHandler -> AddressAndKey -> IO Word64
noteItemLoad h ak = atomicModifyIORef' (schLoadedKeys h) $ \s ->
  if S.member ak s then (s, 0) else (S.insert ak s, kDBRPCCost)

-- | Populate an unmodified view's snapshot base if not yet loaded.
ensureLoaded :: SystemCallHandler -> AddressAndKey -> ItemView -> IO (SystemStatus (SnapshotStorageItem, ItemView))
ensureLoaded h ak v = case ivState v of
  Unmodified (Just base) -> pure (Right (base, v))
  _ -> do
    r <- tryGetSnapshotItem (schSnapshot h) ak
    case r of
      Left e -> pure (Left e)
      Right base -> do
        let v' = case ivState v of
              Unmodified Nothing -> v {ivState = Unmodified (Just base)}
              _ -> v
        pure (Right (base, v'))

isLoaded :: ItemView -> Bool
isLoaded v = case ivState v of
  Unmodified (Just _) -> True
  _ -> False

activeDataKey :: SystemCallHandler -> StorageKeyContractData -> IO AddressAndKey
activeDataKey h k = (`contractDataKey` k) <$> getCurrentAddress h

getBytestring :: SystemCallHandler -> StorageKeyContractData -> IO (SystemStatus (StorageRead B.ByteString))
getBytestring h key = do
  ak <- activeDataKey h key
  view <- getItemView h ak
  case ivState view of
    BytestringItem b -> pure (Right (StorageRead (Right b) 0))
    DeletedItem -> pure (Right (StorageRead (Left KSyscallErrorCodeStorageKeyNotExist) 0))
    Unmodified _ -> do
      loadGas <- if isLoaded view then pure 0 else noteItemLoad h ak
      loaded <- ensureLoaded h ak view
      case loaded of
        Left e -> pure (Left e)
        Right (base, view') -> do
          putItemView h ak view'
          pure . Right $ case ssiEntry base of
            SEBytestring b -> StorageRead (Right b) loadGas
            _ -> case snapshotItemType base of
              Nothing -> StorageRead (Left KSyscallErrorCodeStorageKeyNotExist) loadGas
              Just _ -> StorageRead (Left KSyscallErrorCodeInvalidSyscallInput) loadGas
    _ -> pure (Right (StorageRead (Left KSyscallErrorCodeInvalidSyscallInput) 0))

setBytestring :: SystemCallHandler -> StorageKeyContractData -> B.ByteString -> IO (SystemStatus (SyscallStatus ()))
setBytestring h key bytes = do
  ro <- isReadOnly h
  checkM (not ro) "set_bytestring in a read-only frame"
  if B.length bytes > fromIntegral (schConfig h).max_bytestring_len
    then pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
    else do
      ak <- activeDataKey h key
      view <- getItemView h ak
      r <- setItem (BytestringItem bytes) view
      Right <$> applyView h ak r

-- | Store an updated view, passing through the syscall status.
applyView :: SystemCallHandler -> AddressAndKey -> SyscallStatus ItemView -> IO (SyscallStatus ())
applyView h ak = \case
  Left e -> pure (Left e)
  Right v -> Right <$> putItemView h ak v

-- | The base value (if known) and add value of an nnint.  With
-- load_from_snapshot the base comes from the snapshot when no set value is
-- pending.
getNnint :: SystemCallHandler -> StorageKeyContractData -> Bool -> IO (SystemStatus (StorageRead (Maybe Integer, Integer)))
getNnint h key fromSnapshot = do
  ak <- activeDataKey h key
  view <- getItemView h ak
  case ivState view of
    NNIntItem (Just base) add -> pure (Right (StorageRead (Right (Just base, add)) 0))
    NNIntItem Nothing add
      | not fromSnapshot -> pure (Right (StorageRead (Right (Nothing, add)) 0))
      | otherwise -> do
          loadGas <- noteItemLoad h ak
          snap <- tryGetSnapshotItem (schSnapshot h) ak
          case snap of
            Left e -> pure (Left e)
            Right s -> pure . Right $ case ssiEntry s of
              SENNInt v -> StorageRead (Right (Just v, add)) loadGas
              _ -> StorageRead (Right (Just 0, add)) loadGas
    DeletedItem
      | not fromSnapshot -> pure (Right (StorageRead (Right (Nothing, 0)) 0))
      | otherwise -> pure (Right (StorageRead (Left KSyscallErrorCodeStorageKeyNotExist) 0))
    Unmodified _
      | not fromSnapshot -> pure (Right (StorageRead (Right (Nothing, 0)) 0))
      | otherwise -> do
          loadGas <- if isLoaded view then pure 0 else noteItemLoad h ak
          loaded <- ensureLoaded h ak view
          case loaded of
            Left e -> pure (Left e)
            Right (base, view') -> do
              putItemView h ak view'
              pure . Right $ case ssiEntry base of
                SENNInt v -> StorageRead (Right (Just v, 0)) loadGas
                _ -> case snapshotItemType base of
                  Nothing -> StorageRead (Left KSyscallErrorCodeStorageKeyNotExist) loadGas
                  Just _ -> StorageRead (Left KSyscallErrorCodeInvalidSyscallInput) loadGas
    _
      | not fromSnapshot -> pure (Right (StorageRead (Right (Nothing, 0)) 0))
      | otherwise -> pure (Right (StorageRead (Left KSyscallErrorCodeInvalidSyscallInput) 0))

setNnint :: SystemCallHandler -> StorageKeyContractData -> Maybe Integer -> Integer -> IO (SystemStatus (SyscallStatus ()))
setNnint h key setValue addValue = do
  ro <- isReadOnly h
  checkM (not ro) "set_nnint in a read-only frame"
  ak <- activeDataKey h key
  view <- getItemView h ak
  r <- setItem (NNIntItem setValue addValue) view
  Right <$> applyView h ak r

deleteObject :: SystemCallHandler -> StorageKeyContractData -> IO (SystemStatus (SyscallStatus ()))
deleteObject h key = do
  ro <- isReadOnly h
  checkM (not ro) "delete_object in a read-only frame"
  ak <- activeDataKey h key
  view <- getItemView h ak
  r <- setItem DeletedItem view
  Right <$> applyView h ak r

-- | Only ExclusiveLock is a settable flag.
setStorageFlags :: SystemCallHandler -> StorageKeyContractData -> StorageDeltaFlagSet -> IO (SyscallStatus ())
setStorageFlags h key flags = do
  ro <- isReadOnly h
  checkM (not ro) "set_storage_flags in a read-only frame"
  let StorageDeltaFlags allowed = ExclusiveLock
  if flags .&. fromIntegral allowed /= flags
    then pure (Left KSyscallErrorCodeInvalidSyscallInput)
    else do
      ak <- activeDataKey h key
      view <- getItemView h ak
      applyView h ak (setFlags flags view)

getStorageFlags :: SystemCallHandler -> StorageKeyContractData -> IO StorageDeltaFlagSet
getStorageFlags h key = do
  ak <- activeDataKey h key
  ivFlags <$> getItemView h ak

clearStorageFlags :: SystemCallHandler -> StorageKeyContractData -> IO ()
clearStorageFlags h key = do
  ro <- isReadOnly h
  checkM (not ro) "clear_storage_flags in a read-only frame"
  ak <- activeDataKey h key
  view <- getItemView h ak
  putItemView h ak (clearFlags view)

------------------------------------------------------------------------------
-- Retrocausal queues

retrocausallyConsumedEntries :: SystemCallHandler -> SnapshotStorageItem -> AddressAndKey -> Word32 -> IO [QueueEntry]
retrocausallyConsumedEntries h base ak count = case schMode h of
  ModeValidation v -> getObservedQueueEntries v ak count
  ModeViewing v -> viewGetObservedQueueEntries v ak count
  ModeProposal _ -> getRetrocausallyConsumedEntries base count

-- | A fresh queue over a fully consumed view of the snapshot's queue, for a
-- key this transaction replaced.
makeReplacedKeyQueueItem :: SystemCallHandler -> AddressAndKey -> Word32 -> IO (SystemStatus RetrocausalQueueItem)
makeReplacedKeyQueueItem h ak consumedCount = do
  r <- tryGetSnapshotItem (schSnapshot h) ak
  case r of
    Left e -> pure (Left e)
    Right base -> do
      consumed <- retrocausallyConsumedEntries h base ak consumedCount
      Right <$> makeFullyConsumedItem base consumed

-- | Run a queue read and map an empty result to the right error.
loadNextQueueEntry :: SystemCallHandler -> Bool -> RetrocausalQueueItem -> IO (SystemStatus (SyscallStatus QueueEntry, RetrocausalQueueItem))
loadNextQueueEntry h advance q = do
  r <- rcqGetNextItem (schMode h) advance q
  case r of
    Left e -> pure (Left e)
    Right (Just e, q') -> pure (Right (Right e, q'))
    Right (Nothing, q') -> do
      capped <- observationCapExceeded (schMode h)
      pure
        ( Right
            ( Left (if capped then KSyscallErrorCodeRetrocausalQueueReadLimitReached else KSyscallErrorCodeNoRetrocausalQueueEntry)
            , q'
            )
        )

-- | A read never converts a key, so a non-queue base is wrong-type input.
committedValueIsNotAQueue :: SnapshotStorageItem -> Bool
committedValueIsNotAQueue base = case snapshotItemType base of
  Just t -> t /= IVRetrocausalQueue
  Nothing -> False

retrocausalQueueInsert :: SystemCallHandler -> StorageKeyContractData -> B.ByteString -> IO (SystemStatus (SyscallStatus ()))
retrocausalQueueInsert h key entry
  | B.length entry > kQueueEntryMaxLen = pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
  | otherwise = do
      ak <- activeDataKey h key
      view <- getItemView h ak
      qr <- case ivState view of
        RcqItem q -> pure (Right (Right (q, view)))
        Unmodified _ -> do
          loaded <- ensureLoaded h ak view
          case loaded of
            Left e -> pure (Left e)
            Right (base, view') -> do
              sv <- setItem (RcqItem (newRcqItem base)) view'
              pure (Right (fmap (\v -> (newRcqItem base, v)) sv))
        _ -> do
          -- A write replaces whatever the key held: start a fresh queue over
          -- a fully consumed view of the snapshot.
          nq <- makeReplacedKeyQueueItem h ak (ivRetroConsumed view)
          case nq of
            Left e -> pure (Left e)
            Right q -> do
              sv <- setItem (RcqItem q) view
              pure (Right (fmap (\v -> (q, v)) sv))
      case qr of
        Left e -> pure (Left e)
        Right (Left e) -> pure (Right (Left e))
        Right (Right (q, v)) -> case rcqInsertEntry entry q of
          Left e -> do
            putItemView h ak v
            pure (Right (Left e))
          Right q' -> do
            putItemView h ak v {ivState = RcqItem q'}
            pure (Right (Right ()))

retrocausalQueueConsume :: SystemCallHandler -> StorageKeyContractData -> Bool -> IO (SystemStatus (SyscallStatus QueueEntry))
retrocausalQueueConsume h key peek = do
  ak <- activeDataKey h key
  view <- getItemView h ak
  if peek
    then case ivState view of
      RcqItem q -> fmap fst <$> loadNextQueueEntry h False q
      Unmodified _ -> do
        loaded <- ensureLoaded h ak view
        case loaded of
          Left e -> pure (Left e)
          Right (base, view') -> do
            putItemView h ak view'
            if committedValueIsNotAQueue base
              then pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
              else fmap fst <$> loadNextQueueEntry h False (newRcqItem base)
      DeletedItem -> do
        nq <- makeReplacedKeyQueueItem h ak (ivRetroConsumed view)
        case nq of
          Left e -> pure (Left e)
          Right q -> fmap fst <$> loadNextQueueEntry h False q
      _ -> pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
    else case ivState view of
      -- Probe with a local item; persist only if an entry is found, so a
      -- no-op consume does not pin the storage type.
      Unmodified _ -> do
        loaded <- ensureLoaded h ak view
        case loaded of
          Left e -> pure (Left e)
          Right (base, view') -> do
            putItemView h ak view'
            if committedValueIsNotAQueue base
              then pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
              else consumeAndPersist ak view' (newRcqItem base)
      RcqItem q -> do
        r <- loadNextQueueEntry h True q
        case r of
          Left e -> pure (Left e)
          Right (res, q') -> do
            putItemView h ak (syncRetrocausallyConsumed q' view {ivState = RcqItem q'})
            pure (Right res)
      DeletedItem -> do
        nq <- makeReplacedKeyQueueItem h ak (ivRetroConsumed view)
        case nq of
          Left e -> pure (Left e)
          Right q -> consumeAndPersist ak view q
      _ -> pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
  where
    consumeAndPersist ak view q = do
      r <- loadNextQueueEntry h True q
      case r of
        Left e -> pure (Left e)
        Right (Right entry, q') -> do
          sv <- setItem (RcqItem q') view
          case sv of
            Left e -> pure (Right (Left e))
            Right v -> do
              putItemView h ak (syncRetrocausallyConsumed q' v)
              pure (Right (Right entry))
        Right (Left e, _) -> pure (Right (Left e))

------------------------------------------------------------------------------
-- Native token

nativeBalanceKey :: ContractAddress -> AddressAndKey
nativeBalanceKey addr = AddressAndKey {address = addr, key = StorageKey_FreeNativeTokenBalance}

-- | The load cost get_native_token_balance would charge now, without loading.
peekNativeTokenBalanceLoadGas :: SystemCallHandler -> ContractAddress -> IO Word64
peekNativeTokenBalanceLoadGas h addr = do
  let ak = nativeBalanceKey addr
  view <- getItemView h ak
  let free = case ivState view of
        Unmodified (Just _) -> True
        NativeTokenBalanceItem (Just _) _ -> True
        _ -> False
  if free
    then pure 0
    else do
      loaded <- readIORef (schLoadedKeys h)
      pure (if S.member ak loaded then 0 else kDBRPCCost)

getNativeTokenBalance :: SystemCallHandler -> ContractAddress -> IO (SystemStatus (StorageRead Integer))
getNativeTokenBalance h addr = do
  let ak = nativeBalanceKey addr
  view <- getItemView h ak
  case ivState view of
    Unmodified _ -> do
      loadGas <- if isLoaded view then pure 0 else noteItemLoad h ak
      loaded <- ensureLoaded h ak view
      case loaded of
        Left e -> pure (Left e)
        Right (base, view') -> do
          putItemView h ak view'
          case ssiEntry base of
            SENativeTokenBalance b -> do
              -- A negative committed row can only come from corruption.
              checkM (b >= 0) "negative native token balance in snapshot"
              pure (Right (StorageRead (Right b) loadGas))
            other -> panic ("native token balance key holds " ++ show other)
    NativeTokenBalanceItem mbase add -> do
      (base, loadGas) <- case mbase of
        Just b -> pure (Right b, 0)
        Nothing -> do
          lg <- noteItemLoad h ak
          snap <- tryGetSnapshotItem (schSnapshot h) ak
          pure $ case snap of
            Left e -> (Left e, lg)
            Right s -> case ssiEntry s of
              SENativeTokenBalance b -> (Right b, lg)
              _ -> (Right 0, lg)
      case base of
        Left e -> pure (Left e)
        Right b
          | addOverflows128 b add -> pure (Right (StorageRead (Left KSyscallErrorCodeInvalidSyscallInput) loadGas))
          | b + add < 0 -> pure (Right (StorageRead (Left KSyscallErrorCodeInvalidSyscallInput) loadGas))
          | otherwise -> pure (Right (StorageRead (Right (b + add)) loadGas))
    _ -> panic "native token balance view of unexpected type"

modifyNativeTokenBalance :: SystemCallHandler -> ContractAddress -> Integer -> IO (SystemStatus (SyscallStatus ()))
modifyNativeTokenBalance h addr amount = do
  ro <- isReadOnly h
  if ro
    then pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
    else do
      let ak = nativeBalanceKey addr
      view <- getItemView h ak
      case ivState view of
        NativeTokenBalanceItem base add
          | addOverflows128 add amount -> pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
          | otherwise -> do
              r <- setItem (NativeTokenBalanceItem base (add + amount)) view
              Right <$> applyView h ak r
        _ -> do
          r <- setItem (NativeTokenBalanceItem Nothing amount) view
          Right <$> applyView h ak r

------------------------------------------------------------------------------
-- Authentication

authDataKey :: ContractAddress -> AddressAndKey
authDataKey addr = AddressAndKey {address = addr, key = StorageKey_AuthData}

-- | Apply a mutation to the current address's pending auth-data item.
modifyAuthData
  :: SystemCallHandler
  -> (M.Map Word32 PublicKeyConfigOpt -> Maybe Weight -> (M.Map Word32 PublicKeyConfigOpt, Maybe Weight))
  -> IO (SystemStatus (SyscallStatus ()))
modifyAuthData h mutate = do
  addr <- getCurrentAddress h
  let ak = authDataKey addr
  view <- getItemView h ak
  case ivState view of
    AddressAuthDataItem changes w -> do
      let (c', w') = mutate changes w
      putItemView h ak view {ivState = AddressAuthDataItem c' w'}
      pure (Right (Right ()))
    DeletedItem -> do
      logError "Unrecoverable: unexpected manual delete of authentication data"
      pure (Left SysUnrecoverable)
    _ -> do
      let (c', w') = mutate M.empty Nothing
      r <- setItem (AddressAuthDataItem c' w') view
      Right <$> applyView h ak r

changePublicKeyRegistration :: SystemCallHandler -> Word32 -> PublicKeyConfigOpt -> IO (SystemStatus (SyscallStatus ()))
changePublicKeyRegistration h idx newKey
  | idx >= kMaxSigners = pure (Right (Left KSyscallErrorCodeInvalidSyscallInput))
  | otherwise = modifyAuthData h (\c w -> (M.insert idx newKey c, w))

setAddressRequiredWeight :: SystemCallHandler -> Weight -> IO (SystemStatus (SyscallStatus ()))
setAddressRequiredWeight h w = modifyAuthData h (\c _ -> (c, Just w))

mintAuthToken :: SystemCallHandler -> AuthToken -> IO (SyscallStatus Word32)
mintAuthToken = mintAuthTokenStack

consumeAuthToken :: SystemCallHandler -> Word32 -> BlockHeight -> IO (SyscallStatus ConsumeAuthTokenResult)
consumeAuthToken h tid height = do
  f <- activeFrame h
  case frameLookupAuthToken tid f of
    Nothing -> pure (Left KSyscallErrorCodeInvalidSyscallInput)
    Just t
      | t.expiry_height < height -> pure (Left KSyscallErrorCodeInvalidSyscallInput)
      | otherwise -> consumeAuthTokenStack h tid

------------------------------------------------------------------------------
-- Contracts

-- | Attachments first, then the snapshot cache.
resolveContract :: SystemCallHandler -> SHA256Hash -> IO (SystemStatus (Maybe Contract))
resolveContract h hsh = do
  local <- M.lookup hsh <$> readIORef (schNewContracts h)
  case local of
    Just (c, _) -> pure (Right (Just c))
    Nothing -> fmap (fmap cceContract) <$> tryGetContractEntry (schSnapshot h) hsh

-- | The image already built for @hash@, never a second one.
resolvePreparedContract :: SystemCallHandler -> SHA256Hash -> IO (SystemStatus (Maybe PreparedContract))
resolvePreparedContract h hsh = do
  local <- M.lookup hsh <$> readIORef (schNewContracts h)
  case local of
    Just (_, p) -> pure (Right (Just p))
    Nothing -> fmap (fmap ccePrepared) <$> tryGetContractEntry (schSnapshot h) hsh

-- | The runtime for the contract at an address: a snapshot deployment, a
-- deployment made earlier in this transaction, or -- for the root
-- invocation of an address with no contract -- the chain's default wallet.
resolveRuntime :: SystemCallHandler -> ContractAddress -> IO (SystemStatus (Maybe Runtime))
resolveRuntime h addr = do
  dep <- tryGetDeployedContract (schSnapshot h) addr
  case dep of
    Left e -> pure (Left e)
    Right (Just d) -> rcMakeNewRuntime (schRuntimeContext h) d h
    Right Nothing -> do
      local <- lookupLocallyDeployedContract h addr
      empty <- stackEmpty h
      hsh <- case local of
        Just l -> pure (Just l)
        Nothing
          | empty -> Just . (\t -> t.default_wallet) <$> getDbTip (schSnapshot h)
          | otherwise -> pure Nothing
      case hsh of
        Nothing -> pure (Right Nothing)
        Just dh -> rcMakeNewRuntime (schRuntimeContext h) (DeployedContract dh Nothing) h
