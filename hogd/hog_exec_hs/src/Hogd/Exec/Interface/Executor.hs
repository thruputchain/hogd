-- | The executor's coordinator-facing side (interface/executor*.{h,cc},
-- interface/executor_context.*, interface/executor_state_manager.*,
-- interface/rpc_from_coord.*).
module Hogd.Exec.Interface.Executor
  ( -- * Block contexts
    ExecutorContext
  , ecMode
  , proposeTransaction
  , validateTransaction
  , awaitEventReturn
    -- * State manager
  , ExecutorStateManager
  , newExecutorStateManager
  , openBlock
  , removeBlock
  , withExecutorContext
    -- * The RPC service
  , ExecutorRpc
  , newExecutorRpc
  , initExecutorRpc
  , executorRpcService
  , awaitShutdownRequest
  , registerWithCoordinator
  , kMaxBytesTransaction
  ) where

import Control.Concurrent
import Control.Concurrent.STM
import Control.Exception (SomeException, try)
import Control.Monad
import qualified Data.ByteString.Char8 as BC
import Data.IORef
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ExecutionMode
import Hogd.Exec.Log
import Hogd.Exec.Precompiles.Context (makeHogExecContext)
import Hogd.Exec.Runtime.EventServerProxy
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.StorageSnapshotCache
import Hogd.Exec.Runtime.SyscallHandler
import Hogd.Exec.Runtime.Types (RuntimeContext, SystemCallHandler)
import Hogd.Version (protocolToken)
import Hogd.Xdr
import X23.Rpc
import X23.Xdr (xdrSize)

-- | Cap on the Transaction body, what TransactionHash commits to (#3844).
kMaxBytesTransaction :: Int
kMaxBytesTransaction = 1900000

------------------------------------------------------------------------------
-- ExecutorContext: one block

data ExecutorContext = ExecutorContext
  { ecMode :: !ExecutionMode
  , ecBlockId :: !BlockID
  , ecDb :: DatabaseProxy
  , ecEvents :: EventServerProxy
  , ecCache :: StorageSnapshotCache
  , ecRuntime :: RuntimeContext
  , ecEventError :: IORef (Maybe SystemErrorCode)
  , ecPendingEvents :: TVar Int
  }

newExecutorContext :: ExecutionMode -> BlockID -> DatabaseProxy -> StorageSnapshotCache -> EventServerProxy -> RuntimeContext -> IO ExecutorContext
newExecutorContext mode bid db cache ev rc =
  ExecutorContext mode bid db ev cache rc <$> newIORef Nothing <*> newTVarIO 0

perTx :: ExecutorContext -> TransactionID -> IO (PerTransactionDatabaseProxy, PerTransactionEventServerProxy)
perTx ctx tx =
  (,) <$> newPerTransactionDatabaseProxy (ecDb ctx) (ecBlockId ctx) tx
    <*> newPerTransactionEventServerProxy (ecEvents ctx) (ecBlockId ctx) tx

-- | Stale-block execution during propose/validate is benign: a stale
-- proposal is never committed, and validation replays symmetrically.
proposeTransaction :: ExecutorContext -> SignedTransaction -> TransactionID -> IO ExecProposeResult
proposeTransaction ctx stx txId
  | xdrSize stx.tx > kMaxBytesTransaction = pure (ExecProposeResult_default ExecResultMalformedTransaction)
  | otherwise = do
      (db, ev) <- perTx ctx txId
      h <- newSystemCallHandler (ecRuntime ctx) (ecCache ctx) db ev ProposalModeCtor stx.tx
      _ <- rootInvoke h
      res <- tryCommitTransaction h
      if committed res
        then do
          pd <- extractProposalData h
          spawnEventEmission ctx h res ProposedTransaction {signed_tx = stx, nondeterministic_proposal_data = pd}
          pure (ExecProposeResult_proposal_data res pd)
        else pure (ExecProposeResult_default res)

committed :: ExecResult -> Bool
committed r = r == ExecResultOK || r == ExecResultTxDeterministicError || r == ExecResultTxRootFrameReverted

validateTransaction :: ExecutorContext -> ProposedTransaction -> TransactionID -> IO ExecResult
validateTransaction ctx ptx txId
  | xdrSize ptx.signed_tx.tx > kMaxBytesTransaction = pure ExecResultTxValidationFailure
  | otherwise = do
      (db, ev) <- perTx ctx txId
      h <- newSystemCallHandler (ecRuntime ctx) (ecCache ctx) db ev (ValidationModeCtor ptx.nondeterministic_proposal_data) ptx.signed_tx.tx
      _ <- rootInvoke h
      res <- tryCommitTransaction h
      when (committed res) $ spawnEventEmission ctx h res ptx
      pure res

-- | Emit a committed transaction's events in the background, tracked so
-- finish_block can drain them.
spawnEventEmission :: ExecutorContext -> SystemCallHandler -> ExecResult -> ProposedTransaction -> IO ()
spawnEventEmission ctx h res ptx = do
  atomically $ modifyTVar' (ecPendingEvents ctx) (+ 1)
  void $ forkIO $ do
    r <- try (emitEvents h res ptx)
    logEventReturn ctx $ case r of
      Left (_ :: SomeException) -> Left SysUnrecoverable
      Right s -> s

logEventReturn :: ExecutorContext -> SystemStatus () -> IO ()
logEventReturn ctx st = do
  case st of
    Left e -> do
      writeIORef (ecEventError ctx) (Just e)
      logError ("event error: " ++ show e)
    Right () -> pure ()
  atomically $ modifyTVar' (ecPendingEvents ctx) (subtract 1)

-- | Block until every event emission has returned.
awaitEventReturn :: ExecutorContext -> IO (Maybe SystemErrorCode)
awaitEventReturn ctx = do
  atomically $ readTVar (ecPendingEvents ctx) >>= \n -> unless (n == 0) retry
  readIORef (ecEventError ctx)

------------------------------------------------------------------------------
-- Reference counting (PerThreadARC)

data Arc = Arc {arcCount :: !Int, arcShutdown :: !Bool}

------------------------------------------------------------------------------
-- ExecutorStateManager

data ActiveBlock = ActiveBlock
  { abContext :: ExecutorContext
  , abArc :: IORef Arc
  , abOpenCount :: IORef Int
  }

data ExecutorStateManager = ExecutorStateManager
  { esmDb :: DatabaseProxy
  , esmEvents :: EventServerProxy
  , esmRuntime :: RuntimeContext
  , esmLock :: MVar ()
  , esmBlocks :: IORef (M.Map BlockID ActiveBlock)
  , esmFreeCaches :: IORef [StorageSnapshotCache]
  }

kMaxCachedSnapshotCaches :: Int
kMaxCachedSnapshotCaches = 16

newExecutorStateManager :: DatabaseProxy -> EventServerProxy -> IO ExecutorStateManager
newExecutorStateManager db ev =
  ExecutorStateManager db ev makeHogExecContext <$> newMVar () <*> newIORef M.empty <*> newIORef []

getSnapshotCache :: ExecutorStateManager -> BlockID -> BlockExecutionContext -> IO StorageSnapshotCache
getSnapshotCache m bid ctx = do
  free <- readIORef (esmFreeCaches m)
  case free of
    [] -> newStorageSnapshotCache bid ctx (esmDb m)
    (c : rest) -> do
      writeIORef (esmFreeCaches m) rest
      setCacheBlockId c bid
      setBlockExecutionContext c ctx
      pure c

stashSnapshotCache :: ExecutorStateManager -> StorageSnapshotCache -> IO ()
stashSnapshotCache m c = do
  resetCache c
  modifyIORef' (esmFreeCaches m) (\cs -> if length cs < kMaxCachedSnapshotCaches then c : cs else cs)

-- | Open (or add a reference to) a block in the given mode.
openBlock :: ExecutorStateManager -> BlockID -> ExecutionMode -> BlockExecutionContext -> IO ExecResult
openBlock m bid mode bctx = withMVar (esmLock m) $ \_ -> do
  blocks <- readIORef (esmBlocks m)
  case M.lookup bid blocks of
    Just ab -> do
      open <- blockExecutionContext (ecCache (abContext ab))
      count <- readIORef (abOpenCount ab)
      if ecMode (abContext ab) /= mode
        then pure ExecResultSystemError
        else
          if open.block_height /= bctx.block_height || open.block_timepoint /= bctx.block_timepoint
            then do
              logError
                ( "Tried to reopen block " ++ show bid ++ " with a different execution context (height "
                    ++ show open.block_height ++ " vs " ++ show bctx.block_height ++ ", timepoint "
                    ++ show open.block_timepoint ++ " vs " ++ show bctx.block_timepoint ++ ")"
                )
              pure ExecResultSystemError
            else
              if count <= 0
                then do
                  logError ("Tried to reopen block " ++ show bid ++ " that is closing")
                  pure ExecResultSystemError
                else do
                  writeIORef (abOpenCount ab) (count + 1)
                  logTrace ("Increased ref count on existing block " ++ show bid ++ " in mode " ++ show mode)
                  pure ExecResultOK
    Nothing -> do
      cache <- getSnapshotCache m bid bctx
      tip <- loadDbTip cache
      case tip of
        Left e -> do
          logError ("Failed to load db tip for block " ++ show bid ++ ": " ++ show e)
          pure ExecResultSystemError
        Right () -> do
          ctx <- newExecutorContext mode bid (esmDb m) cache (esmEvents m) (esmRuntime m)
          ab <- ActiveBlock ctx <$> newIORef (Arc 0 False) <*> newIORef 1
          modifyIORef' (esmBlocks m) (M.insert bid ab)
          r <- eventOpenBlock (esmEvents m) bid
          case r of
            Left _ -> do
              logError ("Failed to notify event server of block open: " ++ show bid)
              stashSnapshotCache m cache
              modifyIORef' (esmBlocks m) (M.delete bid)
              pure ExecResultSystemError
            Right () -> do
              logTrace ("Opened new block " ++ show bid ++ " in mode " ++ show mode)
              pure ExecResultOK

-- | Drop one open reference; the last one shuts the block down.
removeBlock :: ExecutorStateManager -> BlockID -> IO ExecResult
removeBlock m bid = do
  logTrace ("Removing reference to block " ++ show bid)
  (result, doGc) <- withMVar (esmLock m) $ \_ -> do
    blocks <- readIORef (esmBlocks m)
    case M.lookup bid blocks of
      Nothing -> do
        logError ("Tried to remove block " ++ show bid ++ " that does not exist")
        pure (ExecResultSystemError, False)
      Just ab -> do
        count <- readIORef (abOpenCount ab)
        if count <= 0
          then do
            logError ("Tried to remove block " ++ show bid ++ " that has a non-positive open_block_count")
            pure (ExecResultSystemError, False)
          else do
            writeIORef (abOpenCount ab) (count - 1)
            if count - 1 /= 0
              then pure (ExecResultOK, False)
              else do
                doGc <- atomicModifyIORef' (abArc ab) $ \a ->
                  if arcShutdown a then (a, False) else (a {arcShutdown = True}, arcCount a == 0)
                evErr <- awaitEventReturn (abContext ab)
                closed <- eventCloseBlock (esmEvents m) bid
                let r0 = case closed of
                      Left _ -> ExecResultSystemError
                      Right () -> ExecResultOK
                case closed of
                  Left _ -> logError ("Failed to notify event server of block close: " ++ show bid)
                  Right () -> pure ()
                r1 <- case evErr of
                  Nothing -> pure r0
                  Just SystemErrorCode_Abandon -> pure ExecResultSystemError
                  Just SystemErrorCode_ValidationFailure -> pure ExecResultTxValidationFailure
                  Just e -> do
                    logError ("Unrecoverable: Received system unrecoverable error code from event server: " ++ show e)
                    pure ExecResultUnrecoverable
                pure (r1, doGc)
  if doGc
    then do
      g <- gcExecutorContext m bid
      case g of
        Left e -> do
          logError ("Unrecoverable: late event-server error " ++ show e ++ " during gc of block " ++ show bid)
          pure ExecResultUnrecoverable
        Right () -> pure result
    else pure result

-- | Recycle a block whose last reference is gone.
gcExecutorContext :: ExecutorStateManager -> BlockID -> IO (SystemStatus ())
gcExecutorContext m bid = withMVar (esmLock m) $ \_ -> do
  logTrace ("Garbage collecting state for block " ++ show bid)
  blocks <- readIORef (esmBlocks m)
  case M.lookup bid blocks of
    Nothing -> pure (Right ())
    Just ab -> do
      evErr <- awaitEventReturn (abContext ab)
      stashSnapshotCache m (ecCache (abContext ab))
      modifyIORef' (esmBlocks m) (M.delete bid)
      case evErr of
        Nothing -> pure (Right ())
        Just SystemErrorCode_Abandon -> do
          logWarn ("Dropped events for block " ++ show bid ++ ": a straggler emit was rejected with Abandon after close_block (working-as-intended)")
          pure (Right ())
        Just e -> pure (Left e)

-- | Acquire a context in the given mode, run an action, release.  Nothing
-- if there is no usable context; Left if releasing surfaced a late
-- event-server error.
withExecutorContext :: ExecutorStateManager -> BlockID -> ExecutionMode -> (ExecutorContext -> IO a) -> IO (SystemStatus (Maybe a))
withExecutorContext m bid mode act = do
  acquired <- withMVar (esmLock m) $ \_ -> do
    blocks <- readIORef (esmBlocks m)
    case M.lookup bid blocks of
      Nothing -> pure Nothing
      Just ab -> do
        ok <- atomicModifyIORef' (abArc ab) $ \a ->
          if arcShutdown a then (a, False) else (a {arcCount = arcCount a + 1}, True)
        pure (if ok then Just ab else Nothing)
  case acquired of
    Nothing -> pure (Right Nothing)
    Just ab
      | ecMode (abContext ab) /= mode -> do
          logWarn
            ( "Expected executor context for block " ++ show bid ++ " to be in " ++ show mode
                ++ " mode, but found in " ++ show (ecMode (abContext ab)) ++ " mode"
            )
          rel <- release ab
          pure (fmap (const Nothing) rel)
      | otherwise -> do
          a <- act (abContext ab)
          rel <- release ab
          pure (fmap (const (Just a)) rel)
  where
    release ab = do
      lastRef <- atomicModifyIORef' (abArc ab) $ \a ->
        let a' = a {arcCount = arcCount a - 1} in (a', arcShutdown a' && arcCount a' == 0)
      if lastRef then gcExecutorContext m bid else pure (Right ())

------------------------------------------------------------------------------
-- The ExecutorSched RPC service (rpc_from_coord)

data ExecutorRpc = ExecutorRpc
  { erStates :: IORef (Maybe ExecutorStateManager)
  , erExecId :: IORef Word64
  , erNextTx :: IORef Word64
  , erShutdown :: MVar ()
  }

newExecutorRpc :: IO ExecutorRpc
newExecutorRpc = ExecutorRpc <$> newIORef Nothing <*> newIORef maxBound <*> newIORef 0 <*> newEmptyMVar

-- | Publish the state the handlers read; must finish before serving.
initExecutorRpc :: ExecutorRpc -> IntroExecResult -> IO ()
initExecutorRpc r intro = do
  when (intro.exec_id >= 0xffff) $ panic "ExecutorMempoolRpc: exec_id is too large"
  writeIORef (erExecId r) intro.exec_id
  db <- newDatabaseProxy (ShardedDB intro.shards) 0x7fffffff
  ev <- eventServerFromCard intro.event
  m <- newExecutorStateManager db ev
  writeIORef (erStates r) (Just m)

awaitShutdownRequest :: ExecutorRpc -> IO ()
awaitShutdownRequest = readMVar . erShutdown

-- | Transaction ids: a contiguous block tagged with the executor id in the
-- top 16 bits.
allocateTxIds :: ExecutorRpc -> Int -> IO TransactionID
allocateTxIds r count = do
  base <- atomicModifyIORef' (erNextTx r) (\v -> (v + fromIntegral count, v))
  checkM (base + fromIntegral count <= 2 ^ (48 :: Int)) "transaction id space exhausted"
  eid <- readIORef (erExecId r)
  pure (base + eid * 2 ^ (48 :: Int))

executorRpcService :: ExecutorRpc -> ExecutorSched1
executorRpcService r =
  ExecutorSched1
    { executorsched_null = pure ()
    , executorsched_ready = maybe ExecResultSystemError (const ExecResultOK) <$> readIORef (erStates r)
    , executorsched_propose_transaction = \bid stx -> do
        tx <- allocateTxIds r 1
        withCtx bid Proposing (ExecProposeResult_default ExecResultUnrecoverable) (ExecProposeResult_default ExecResultSystemError) $ \ctx ->
          proposeTransaction ctx stx tx
    , executorsched_validate_transaction = \bid ptx -> do
        tx <- allocateTxIds r 1
        withCtx bid Validating ExecResultUnrecoverable ExecResultSystemError $ \ctx -> validateTransaction ctx ptx tx
    , executorsched_propose_batch = \bid stxs -> do
        let n = length stxs
        first <- allocateTxIds r n
        withCtx bid Proposing (replicate n (ExecProposeResult_default ExecResultUnrecoverable)) (replicate n (ExecProposeResult_default ExecResultSystemError)) $ \ctx ->
          zipWithM (proposeTransaction ctx) stxs [first ..]
    , executorsched_validate_batch = \bid ptxs -> do
        let n = length ptxs
        first <- allocateTxIds r n
        withCtx bid Validating (replicate n ExecResultUnrecoverable) (replicate n ExecResultSystemError) $ \ctx ->
          zipWithM (validateTransaction ctx) ptxs [first ..]
    , executorsched_open_proposal_block = \bid bctx -> withStates ExecResultSystemError $ \m -> openBlock m bid Proposing bctx
    , executorsched_open_validation_block = \bid bctx -> withStates ExecResultSystemError $ \m -> openBlock m bid Validating bctx
    , executorsched_finish_block = \bid -> withStates ExecResultSystemError $ \m -> removeBlock m bid
    , executorsched_shutdown = void (tryPutMVar (erShutdown r) ())
    }
  where
    withStates dflt act =
      readIORef (erStates r) >>= \case
        Nothing -> pure dflt
        Just m -> act m
    withCtx bid mode unrec missing act =
      readIORef (erStates r) >>= \case
        Nothing -> do
          logWarn "ExecutorMempoolRpc: accessing context before initialization"
          pure missing
        Just m -> do
          res <- withExecutorContext m bid mode act
          pure $ case res of
            Left _ -> unrec
            Right Nothing -> missing
            Right (Just a) -> a

-- | Introduce this executor to the coordinator (retrying until it accepts
-- connections) and return the shard and event-server cards.
registerWithCoordinator :: String -> Word16 -> String -> IO IntroExecResult
registerWithCoordinator coordHost coordPort selfAddress = do
  client <- connectRpcClientRetry coordHost coordPort
  let card = ExecCard {protocol = protocolToken, upid = BC.replicate 8 '\0', address = BC.pack selfAddress}
  r <- call_schedexec_intro_exec client card
  closeRpcClient client
  pure r
