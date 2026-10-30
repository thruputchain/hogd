-- | Read-only invocation server for the public RPC gateway
-- (interface/rpc_from_view_gateway.{h,cc}).
--
-- Views run against a pinned viewing snapshot that is reused across requests
-- and replaced when a background worker sees the committed tip move past it,
-- or when a view is abandoned because its snapshot went stale.
module Hogd.Exec.Interface.ViewGateway
  ( ViewGatewayRpc
  , newViewGatewayRpc
  , viewGatewayService
  , shutDownViewGateway
  , requestRefresh
  , viewGatewayResultFromRaw
  , normalizePublicViewQuery
  ) where

import Control.Concurrent
import Control.Exception (SomeException, try)
import Control.Monad
import qualified Data.ByteString.Char8 as BC
import Data.IORef
import qualified Data.Map.Strict as M
import Data.Time.Clock.POSIX (getPOSIXTime)
import Data.Word
import GHC.Clock (getMonotonicTimeNSec)
import Hogd.Base.NetworkConfigParams (NetworkConfigParams (..))
import Hogd.Exec.Log
import Hogd.Exec.Precompiles.Context (makeHogExecContext)
import Hogd.Exec.Runtime.EventServerProxy
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.StorageSnapshotCache
import Hogd.Exec.Runtime.Types (RuntimeContext)
import Hogd.Exec.Runtime.View
import Hogd.Version (protocolToken)
import Hogd.Xdr.Consensus (ConsensusValue (..))
import Hogd.Xdr.Header (ProposalHeader (..))
import Hogd.Xdr.RpcTypes
import Hogd.Xdr.SchedDb (ShardCard (..))
import Hogd.Xdr.Transaction
import Hogd.Xdr.View
import Hogd.Xdr.ViewGateway
import X23.Rpc (parseHostPort)
import X23.Xdr (xdrSize)

kMaxViewAttempts :: Int
kMaxViewAttempts = 3

kRefreshProbeIntervalMicros :: Int
kRefreshProbeIntervalMicros = 100000

data ViewState = ViewState
  { vsBlockId :: !BlockID
  , vsTip :: !DbTip
  , vsCache :: StorageSnapshotCache
  , vsArc :: IORef (Int, Bool)
  -- ^ references, shutting down
  }

data ViewGatewayRpc = ViewGatewayRpc
  { vgDb :: DatabaseProxy
  , vgEvents :: EventServerProxy
  , vgRuntime :: RuntimeContext
  , vgLock :: MVar ()
  , vgCurrent :: IORef BlockID
  , vgStates :: IORef (M.Map BlockID ViewState)
  , vgShutdown :: IORef Bool
  , vgRefresh :: IORef Bool
  }

-- | A view stands in for a hypothetical block on top of the tip: height is
-- the tip's plus one, and the timepoint is now (Unix nanoseconds).
viewBlockExecutionContext :: DbTip -> IO BlockExecutionContext
viewBlockExecutionContext tip = do
  now <- getPOSIXTime
  pure BlockExecutionContext {block_height = tip.value.proposal_header.block_height + 1, block_timepoint = floor (now * 1e9)}

newViewState :: DatabaseProxy -> BlockID -> DbTip -> IO ViewState
newViewState db bid tip = do
  ctx <- viewBlockExecutionContext tip
  cache <- newStorageSnapshotCache bid ctx db
  setDbTip cache tip
  ViewState bid tip cache <$> newIORef (0, False)

-- | Connect to hog_db and the event server, and start the refresh worker.
newViewGatewayRpc :: String -> String -> Word16 -> IO ViewGatewayRpc
newViewGatewayRpc dbAddr eventHost eventPort = do
  db <- case parseHostPort dbAddr of
    Nothing -> panic "Failed to parse database shard address"
    Just _ ->
      newDatabaseProxy
        ( ShardedDB
            [ ShardCard
                { protocol = protocolToken
                , upid = BC.replicate 8 '\0'
                , block_height = 0
                , address_for_exec = BC.pack dbAddr
                , address_for_sched = mempty
                , address_for_auth = mempty
                }
            ]
        )
        0x7fffffff
  ev <- connectEventServer eventHost eventPort 0x1000000
  g <-
    ViewGatewayRpc db ev makeHogExecContext
      <$> newMVar ()
      <*> newIORef 0
      <*> newIORef M.empty
      <*> newIORef False
      <*> newIORef False
  void $ forkIO (refreshWorker g)
  pure g

requestRefresh :: ViewGatewayRpc -> IO ()
requestRefresh g = writeIORef (vgRefresh g) True

viewGatewayService :: ViewGatewayRpc -> ViewGateway1
viewGatewayService g =
  ViewGateway1
    { viewgateway_intro = \p ->
        pure (if p /= protocolToken then ViewGatewayIntroMismatchedProtocol else ViewGatewayIntroSuccess)
    , viewgateway_view = runViewWithRetries g . normalizePublicViewQuery
    , viewgateway_view_batch = viewBatch g
    }

-- | Views assume auth without signature checks and never commit writes:
-- clear the invoke flags, keep the caller context and forwarded tokens.
normalizePublicViewQuery :: TransactionViewQuery -> TransactionViewQuery
normalizePublicViewQuery q = q {invoke_flags = 0}

viewGatewayResultFromRaw :: BlockHeight -> ExpectedViewResult -> ViewGatewayResult
viewGatewayResultFromRaw height = \case
  ExpectedViewResult_result r ->
    ViewGatewayResult_payload ViewGatewayPayload {height = height, output = r.output, gas_consumed = r.gas_consumed, events = r.events}
  ExpectedViewResult_error_code e -> ViewGatewayResult_error_code e
  ExpectedViewResult_Abandoned Abandoned -> ViewGatewayResult_ViewGatewayAbandoned
  ExpectedViewResult_Abandoned _ -> ViewGatewayResult_ViewGatewayInternalError

gatewayOutcome :: ViewGatewayResult -> ViewGatewayOutcome
gatewayOutcome = discriminant_ViewGatewayResult

------------------------------------------------------------------------------
-- View state management

acquire :: ViewState -> IO Bool
acquire s = atomicModifyIORef' (vsArc s) $ \(n, down) -> if down then ((n, down), False) else ((n + 1, down), True)

-- | True if this dropped the last reference of a shut-down state.
release :: ViewState -> IO Bool
release s = atomicModifyIORef' (vsArc s) $ \(n, down) -> ((n - 1, down), down && n - 1 == 0)

-- | True if the state can be destroyed now.
initiateShutdown :: ViewState -> IO Bool
initiateShutdown s = atomicModifyIORef' (vsArc s) $ \(n, down) -> if down then ((n, down), False) else ((n, True), n == 0)

openSnapshot :: ViewGatewayRpc -> IO (Maybe ViewState)
openSnapshot g = do
  r <- openViewingSnapshot (vgDb g)
  case r of
    Left _ -> pure Nothing
    Right snap -> Just <$> newViewState (vgDb g) snap.viewing_block_id snap.tip

-- | The pinned view state with a reference acquired, opening or refreshing
-- the snapshot as needed.
getCurrentViewState :: ViewGatewayRpc -> IO (Maybe ViewState)
getCurrentViewState g = withMVar (vgLock g) $ \_ -> do
  cur <- readIORef (vgCurrent g)
  refresh <- atomicModifyIORef' (vgRefresh g) (False,)
  when (cur /= 0 && refresh) $ do
    r <- openViewingSnapshot (vgDb g)
    case r of
      Right snap
        | snap.viewing_block_id /= cur -> do
            states <- readIORef (vgStates g)
            canGc <- maybe (pure False) initiateShutdown (M.lookup cur states)
            st <- newViewState (vgDb g) snap.viewing_block_id snap.tip
            modifyIORef' (vgStates g) (M.insertWith (\_ old -> old) snap.viewing_block_id st)
            writeIORef (vgCurrent g) snap.viewing_block_id
            when canGc $ do
              c <- closeViewingSnapshot (vgDb g) cur
              case c of
                Left _ -> logWarn ("viewgateway: failed to close refreshed-out viewing snapshot " ++ show cur)
                Right () -> pure ()
              modifyIORef' (vgStates g) (M.delete cur)
        | otherwise -> pure ()
      Left _ -> logWarn "viewgateway: failed to open refreshed viewing snapshot"
  cur' <- readIORef (vgCurrent g)
  down <- readIORef (vgShutdown g)
  ok <-
    if cur' /= 0
      then pure True
      else
        if down
          then pure False
          else do
            ms <- openSnapshot g
            case ms of
              Nothing -> do
                logWarn "viewgateway: failed to open viewing snapshot"
                pure False
              Just st -> do
                modifyIORef' (vgStates g) (M.insertWith (\_ old -> old) (vsBlockId st) st)
                writeIORef (vgCurrent g) (vsBlockId st)
                pure True
  if not ok
    then pure Nothing
    else do
      c <- readIORef (vgCurrent g)
      states <- readIORef (vgStates g)
      case M.lookup c states of
        Nothing -> do
          logWarn ("viewgateway: current view " ++ show c ++ " missing from view_states_")
          pure Nothing
        Just st -> do
          got <- acquire st
          pure (if got then Just st else Nothing)

-- | Replace a stale (abandoned) snapshot.
rotateViewState :: ViewGatewayRpc -> BlockID -> IO Bool
rotateViewState g stale = withMVar (vgLock g) $ \_ -> do
  cur <- readIORef (vgCurrent g)
  if cur /= stale
    then pure True
    else do
      states <- readIORef (vgStates g)
      mapM_ initiateShutdown (M.lookup stale states)
      ms <- openSnapshot g
      case ms of
        Nothing -> do
          logWarn "viewgateway: failed to open replacement viewing snapshot"
          writeIORef (vgCurrent g) 0
          pure False
        Just st -> do
          modifyIORef' (vgStates g) (M.insertWith (\_ old -> old) (vsBlockId st) st)
          writeIORef (vgCurrent g) (vsBlockId st)
          pure True

gcViewState :: ViewGatewayRpc -> BlockID -> IO ()
gcViewState g bid = do
  c <- closeViewingSnapshot (vgDb g) bid
  case c of
    Left _ -> logWarn ("viewgateway: failed to close viewing snapshot " ++ show bid)
    Right () -> pure ()
  withMVar (vgLock g) $ \_ -> modifyIORef' (vgStates g) (M.delete bid)

releaseState :: ViewGatewayRpc -> ViewState -> IO ()
releaseState g st = do
  lastRef <- release st
  when lastRef $ gcViewState g (vsBlockId st)

------------------------------------------------------------------------------
-- Views

runViewAttempt :: ViewGatewayRpc -> TransactionViewQuery -> ViewState -> IO ViewGatewayResult
runViewAttempt g query st = do
  params <- getNetworkConfigParams (vsCache st)
  if query.invocation.gas_limit > params.max_tx_gas
    then pure (ViewGatewayResult_error_code KSyscallErrorCodeInvalidSyscallInput)
    else do
      db <- newPerTransactionDatabaseProxy (vgDb g) (vsBlockId st) maxBound
      ev <- newPerTransactionEventServerProxy (vgEvents g) (vsBlockId st) maxBound
      -- Empty nondeterministic data: TryInvoke outcomes default to success.
      raw <- runViewTransaction (vsCache st) db ev query (vgRuntime g) emptyProposalData
      pure (viewGatewayResultFromRaw (vsTip st).value.proposal_header.block_height raw)

emptyProposalData :: NondeterministicProposalData
emptyProposalData = NondeterministicProposalData {nondeterministic_bits = mempty, nondeterministic_bit_count = 0, observed_queue_entry_sequence = []}

runViewWithRetries :: ViewGatewayRpc -> TransactionViewQuery -> IO ViewGatewayResult
runViewWithRetries g query = go 0
  where
    go attempt
      | attempt >= kMaxViewAttempts = pure ViewGatewayResult_ViewGatewayAbandoned
      | otherwise = do
          down <- readIORef (vgShutdown g)
          if down
            then do
              logWarn "viewgateway: shutdown requested before view attempt"
              pure ViewGatewayResult_ViewGatewayInternalError
            else
              getCurrentViewState g >>= \case
                Nothing -> pure ViewGatewayResult_ViewGatewayInternalError
                Just st -> do
                  r <- try (runViewAttempt g query st)
                  let result = either (\(_ :: SomeException) -> ViewGatewayResult_ViewGatewayInternalError) id r
                  if gatewayOutcome result /= ViewGatewayAbandoned
                    then releaseState g st >> pure result
                    else do
                      rotated <- rotateViewState g (vsBlockId st)
                      releaseState g st
                      if not rotated
                        then pure ViewGatewayResult_ViewGatewayInternalError
                        else do
                          down' <- readIORef (vgShutdown g)
                          if down'
                            then do
                              logWarn "viewgateway: shutdown requested after abandoned view attempt"
                              pure ViewGatewayResult_ViewGatewayInternalError
                            else go (attempt + 1)

nowMillis :: IO Word64
nowMillis = (`div` 1000000) <$> getMonotonicTimeNSec

batchErr :: ViewGatewayBatchOutcome -> ViewGatewayBatchResult
batchErr = ViewGatewayBatchResult_default

viewBatch :: ViewGatewayRpc -> ViewGatewayBatchQuery -> IO ViewGatewayBatchResult
viewBatch g query0 = do
  received <- nowMillis
  if null query0.calls || query0.budget_millis == 0 || query0.max_response_bytes == 0 || maybe False (< 0) (fmap toInteger query0.min_height)
    then pure (batchErr ViewGatewayBatchInvalidRequest)
    else
      if any (\c -> c.invocation.gas_limit == 0) query0.calls
        then pure (batchErr ViewGatewayBatchInvalidRequest)
        else do
          let query = query0 {calls = map normalizePublicViewQuery query0.calls}
          runBatchWithRetries g query (received + fromIntegral query.budget_millis)

runBatchWithRetries :: ViewGatewayRpc -> ViewGatewayBatchQuery -> Word64 -> IO ViewGatewayBatchResult
runBatchWithRetries g query deadline = go 0
  where
    expired = (>= deadline) <$> nowMillis
    go attempt
      | attempt >= kMaxViewAttempts = do
          e <- expired
          pure (batchErr (if e then ViewGatewayBatchDeadlineExceeded else ViewGatewayBatchAbandoned))
      | otherwise = do
          e <- expired
          down <- readIORef (vgShutdown g)
          if e
            then pure (batchErr ViewGatewayBatchDeadlineExceeded)
            else
              if down
                then pure (batchErr ViewGatewayBatchInternalError)
                else
                  getCurrentViewState g >>= \case
                    Nothing -> pure (batchErr ViewGatewayBatchInternalError)
                    Just st -> do
                      let height = (vsTip st).value.proposal_header.block_height
                      r <- try $ do
                        e' <- expired
                        res <-
                          if e'
                            then pure (batchErr ViewGatewayBatchDeadlineExceeded)
                            else case query.min_height of
                              Just mh
                                | height < mh ->
                                    pure (ViewGatewayBatchResult_height_unavailable ViewGatewayHeightUnavailable {requested = mh, observed = height})
                              _ -> runBatchAttempt g query st deadline
                        e'' <- expired
                        if discriminant_ViewGatewayBatchResult res == ViewGatewayBatchAbandoned && attempt + 1 < kMaxViewAttempts && not e''
                          then do
                            rotated <- rotateViewState g (vsBlockId st)
                            pure (if rotated then res else batchErr ViewGatewayBatchInternalError)
                          else pure res
                      result <- case r of
                        Left (ex :: SomeException) -> do
                          logWarn ("viewgateway: batch failed: " ++ show ex)
                          pure (batchErr ViewGatewayBatchInternalError)
                        Right res -> pure res
                      releaseState g st
                      if discriminant_ViewGatewayBatchResult result /= ViewGatewayBatchAbandoned
                        then pure result
                        else go (attempt + 1)

runBatchAttempt :: ViewGatewayRpc -> ViewGatewayBatchQuery -> ViewState -> Word64 -> IO ViewGatewayBatchResult
runBatchAttempt g query st deadline = do
  params <- getNetworkConfigParams (vsCache st)
  let height = (vsTip st).value.proposal_header.block_height
      responseLimit = min (fromIntegral query.max_response_bytes) (0x7fffffff :: Int)
      gasOk = go params.max_tx_gas query.calls
        where
          go _ [] = True
          go rem' (c : cs) = c.invocation.gas_limit <= rem' && go (rem' - c.invocation.gas_limit) cs
      empty = ViewGatewayBatchResult_payload ViewGatewayBatchPayload {height = height, results = []}
  if not gasOk
    then pure (batchErr ViewGatewayBatchInvalidRequest)
    else do
      let base = xdrSize empty
      if base > responseLimit
        then pure (batchErr ViewGatewayBatchResponseTooLarge)
        else loop base [] query.calls
  where
    loop bytes acc [] = do
      e <- (>= deadline) <$> nowMillis
      pure $
        if e
          then batchErr ViewGatewayBatchDeadlineExceeded
          else
            ViewGatewayBatchResult_payload
              ViewGatewayBatchPayload {height = (vsTip st).value.proposal_header.block_height, results = reverse acc}
    loop bytes acc (call : rest) = do
      down <- readIORef (vgShutdown g)
      e <- (>= deadline) <$> nowMillis
      if down
        then pure (batchErr ViewGatewayBatchInternalError)
        else
          if e
            then pure (batchErr ViewGatewayBatchDeadlineExceeded)
            else do
              result <- runViewAttempt g call st
              case result of
                ViewGatewayResult_payload p -> do
                  let cr = TransactionViewResult {output = p.output, gas_consumed = p.gas_consumed, events = p.events}
                      callBytes = xdrSize cr
                      limit = min (fromIntegral query.max_response_bytes) (0x7fffffff :: Int)
                  if callBytes > limit - bytes
                    then pure (batchErr ViewGatewayBatchResponseTooLarge)
                    else loop (bytes + callBytes) (cr : acc) rest
                ViewGatewayResult_ViewGatewayAbandoned -> pure (batchErr ViewGatewayBatchAbandoned)
                ViewGatewayResult_error_code _ -> pure (batchErr ViewGatewayBatchInvalidRequest)
                _ -> pure (batchErr ViewGatewayBatchInternalError)

------------------------------------------------------------------------------
-- Refresh worker and shutdown

refreshWorker :: ViewGatewayRpc -> IO ()
refreshWorker g = do
  threadDelay kRefreshProbeIntervalMicros
  down <- readIORef (vgShutdown g)
  unless down $ do
    r <- try (probeRefreshTip g)
    case r of
      Left (e :: SomeException) -> logWarn ("viewgateway: refresh probe failed: " ++ show e)
      Right () -> pure ()
    refreshWorker g

currentViewPin :: ViewGatewayRpc -> IO (Maybe (BlockID, BlockHeight))
currentViewPin g = withMVar (vgLock g) $ \_ -> do
  cur <- readIORef (vgCurrent g)
  down <- readIORef (vgShutdown g)
  if cur == 0 || down
    then pure Nothing
    else do
      states <- readIORef (vgStates g)
      case M.lookup cur states of
        Nothing -> do
          logWarn ("viewgateway: current view " ++ show cur ++ " missing from view_states_")
          pure Nothing
        Just st -> pure (Just (vsBlockId st, (vsTip st).value.proposal_header.block_height))

probeRefreshTip :: ViewGatewayRpc -> IO ()
probeRefreshTip g =
  currentViewPin g >>= \case
    Nothing -> pure ()
    Just (bid, viewHeight) -> do
      tipRes <- getDbTip (vgDb g) bid (viewHeight + 1)
      let newer = case tipRes of
            Left _ -> True
            Right t -> t.value.proposal_header.block_height > viewHeight
      when newer $ withMVar (vgLock g) $ \_ -> do
        cur <- readIORef (vgCurrent g)
        down <- readIORef (vgShutdown g)
        unless (cur /= bid || down) $ do
          states <- readIORef (vgStates g)
          case M.lookup bid states of
            Nothing -> logWarn ("viewgateway: view " ++ show bid ++ " missing from view_states_")
            Just st -> do
              let pinned = (vsTip st).value.proposal_header.block_height
              case tipRes of
                Left _ -> requestRefresh g
                Right t | t.value.proposal_header.block_height > pinned -> requestRefresh g
                _ -> pure ()

shutDownViewGateway :: ViewGatewayRpc -> IO ()
shutDownViewGateway g = do
  writeIORef (vgShutdown g) True
  withMVar (vgLock g) $ \_ -> do
    states <- readIORef (vgStates g)
    forM_ (M.toList states) $ \(bid, st) -> do
      canClose <- initiateShutdown st
      when canClose $ do
        c <- closeViewingSnapshot (vgDb g) bid
        case c of
          Left _ -> logWarn ("viewgateway: failed to close viewing snapshot " ++ show bid ++ " on shutdown")
          Right () -> pure ()
        modifyIORef' (vgStates g) (M.delete bid)
    writeIORef (vgCurrent g) 0
