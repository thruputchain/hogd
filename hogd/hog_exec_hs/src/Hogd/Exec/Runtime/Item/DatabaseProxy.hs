-- | The executor's client for hog_db (runtime/item/database_proxy.{h,cc}).
--
-- Maps RPC error codes to the restricted forms the executor handles:
-- kRPCBadBlockID -> Abandon, NotFound -> absent value (or Unrecoverable where
-- a value is mandatory), anything else -> Unrecoverable.  Transport failures
-- never escape: they become Unrecoverable.
module Hogd.Exec.Runtime.Item.DatabaseProxy
  ( DatabaseProxy
  , DatabaseIdentifier (..)
  , newDatabaseProxy
  , getSnapshotItem
  , getContract
  , getDeployment
  , getNextRetrocausalQueueBlock
  , getConcurrentRetrocausalQueueEntry
  , tryPushNewSubtransaction
  , commitTransaction
  , abortTransaction
  , popSubtransactions
  , dbGetDbTip
  , openViewingSnapshot
  , closeViewingSnapshot
    -- * Per transaction
  , PerTransactionDatabaseProxy
  , newPerTransactionDatabaseProxy
  , ptxAddDelta
  , ptxTryPushCurrentSubtransaction
  , ptxPopSubtransactions
  , ptxCommitTransaction
  , ptxAbortTransaction
  , ptxLastPushConflict
  , ptxBlockId
  , ptxTxId
    -- * Size gate (exposed for tests)
  , footprintBound
  , messageOverhead
  , execToDbMessageCap
  ) where

import Control.Concurrent (getNumCapabilities)
import Control.Exception (SomeException, evaluate, try)
import qualified Data.ByteString as B
import Data.IORef
import Data.Maybe (fromMaybe)
import qualified Data.Vector as V
import Data.Word
import GHC.Conc (numCapabilities)
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Xdr
import X23.Rpc

data DatabaseIdentifier
  = LocalMockDB Word16
  -- ^ a server on localhost (tests)
  | ShardedDB [ShardCard]

-- | A pool of multiplexed connections to hog_db.
data DatabaseProxy = DatabaseProxy
  { dpClients :: V.Vector RpcClient
  , dpNext :: IORef Int
  }

-- | Connect.  Like the C++ multiclient, more than one connection is opened so
-- one reader thread does not serialize every reply (db_connection_map.cc
-- sizes it at half a connection per core).
newDatabaseProxy :: DatabaseIdentifier -> Int -> IO DatabaseProxy
newDatabaseProxy ident maxMsg = do
  (host, port, n) <- case ident of
    LocalMockDB p -> pure ("localhost", p, 1)
    ShardedDB [] -> panic "ShardedDB with no shards"
    ShardedDB [s] -> case parseHostPort (bytesToString s.address_for_exec) of
      Nothing -> panic "Failed to parse database shard address"
      Just (h, p) -> do
        caps <- getNumCapabilities
        pure (h, p, max 1 (max caps numCapabilities `div` 2))
    ShardedDB _ -> panic "ShardedDB with more than one shard is not yet supported"
  logInfo ("DatabaseProxy: " ++ show n ++ " connections")
  clients <- V.replicateM n (connectRpcClientWith False maxMsg host port)
  DatabaseProxy clients <$> newIORef 0

bytesToString :: B.ByteString -> String
bytesToString = map (toEnum . fromIntegral) . B.unpack

pickClient :: DatabaseProxy -> IO RpcClient
pickClient dp = do
  i <- atomicModifyIORef' (dpNext dp) (\i -> (i + 1, i))
  pure (dpClients dp V.! (i `mod` V.length (dpClients dp)))

-- | Run an RPC, containing every transport/serialization failure as
-- Unrecoverable (report_rpc_failure).
guarded :: String -> (RpcClient -> IO a) -> DatabaseProxy -> IO (SystemStatus a)
guarded method act dp = do
  c <- pickClient dp
  r <- try (act c >>= evaluate)
  case r of
    Right a -> pure (Right a)
    Left (e :: SomeException) -> do
      logError ("Unrecoverable: " ++ method ++ " failed (transport/serialization/allocation failure): " ++ show e)
      pure (Left SysUnrecoverable)

-- | to_system_error_code for a reply that may legitimately be absent.
mapOptional :: RPCError -> Maybe a -> IO (SystemStatus (Maybe a))
mapOptional err val = case err of
  KRPCNone -> pure (Right val)
  KRPCStorageWriteFailed -> pure (Right Nothing)
  KRPCNotFound -> pure (Right Nothing)
  KRPCBadBlockID -> pure (Left SysAbandon)
  _ -> do
    logError ("Unrecoverable: Received unhandled error type from RPC: " ++ show err)
    pure (Left SysUnrecoverable)

-- | to_system_error_code for a reply with no value.
mapVoid :: RPCError -> IO (SystemStatus ())
mapVoid = \case
  KRPCNone -> pure (Right ())
  KRPCBadBlockID -> pure (Left SysAbandon)
  err@KRPCStorageWriteFailed -> notExpected err
  err@KRPCNotFound -> notExpected err
  err -> do
    logError ("Unrecoverable: Received unhandled error type from RPC: " ++ show err)
    pure (Left SysUnrecoverable)
  where
    notExpected err = do
      logError ("Unrecoverable: Received RPCNotFound or RPCStorageWriteFailed on unexpected rpc: " ++ show err)
      pure (Left SysUnrecoverable)

-- | to_system_error_code with NotFoundIsUnrecoverableError.
mapMandatory :: RPCError -> Maybe a -> IO (SystemStatus a)
mapMandatory err val = case (err, val) of
  (KRPCNone, Just v) -> pure (Right v)
  (KRPCBadBlockID, _) -> pure (Left SysAbandon)
  (KRPCStorageWriteFailed, _) -> notExpected
  (KRPCNotFound, _) -> notExpected
  _ -> do
    logError ("Unrecoverable: Received unhandled error type from RPC: " ++ show err)
    pure (Left SysUnrecoverable)
  where
    notExpected = do
      logError ("Unrecoverable: Received RPCNotFound or RPCStorageWriteFailed on unexpected rpc: " ++ show err)
      pure (Left SysUnrecoverable)

bindSys :: SystemStatus a -> (a -> IO (SystemStatus b)) -> IO (SystemStatus b)
bindSys (Left e) _ = pure (Left e)
bindSys (Right a) f = f a

getSnapshotItem :: DatabaseProxy -> BlockID -> AddressAndKey -> IO (SystemStatus (Maybe WireStorageItem))
getSnapshotItem dp bid ak = do
  r <- guarded "get_snapshot_item" (\c -> call_get_snapshot_item c bid ak) dp
  bindSys r $ \case
    ExpectedStorageItem_item i -> mapOptional KRPCNone (Just i)
    ExpectedStorageItem_default e -> mapOptional e Nothing

getContract :: DatabaseProxy -> BlockID -> SHA256Hash -> IO (SystemStatus (Maybe Contract))
getContract dp bid h = do
  r <- guarded "get_contract" (\c -> call_get_contract c bid h) dp
  bindSys r $ \case
    ExpectedContract_contract x -> mapOptional KRPCNone (Just x)
    ExpectedContract_default e -> mapOptional e Nothing

getDeployment :: DatabaseProxy -> BlockID -> ContractAddress -> IO (SystemStatus (Maybe DeployedContractHash))
getDeployment dp bid addr = do
  r <- guarded "get_deployment" (\c -> call_get_deployment c bid addr) dp
  bindSys r $ \case
    ExpectedDeployment_deployed_hash x -> mapOptional KRPCNone (Just x)
    ExpectedDeployment_default e -> mapOptional e Nothing

getNextRetrocausalQueueBlock :: DatabaseProxy -> BlockID -> AddressAndKey -> Word64 -> IO (SystemStatus (Maybe RetrocausalQueueBlock))
getNextRetrocausalQueueBlock dp bid ak start = do
  r <- guarded "get_next_retrocausal_queue_block" (\c -> call_get_next_retrocausal_queue_block c bid ak start) dp
  bindSys r $ \case
    ExpectedRetrocausalQueueBlock_queue_block x -> mapOptional KRPCNone (Just x)
    ExpectedRetrocausalQueueBlock_default e -> mapOptional e Nothing

getConcurrentRetrocausalQueueEntry :: DatabaseProxy -> BlockID -> AddressAndKey -> Word32 -> IO (SystemStatus (Maybe QueueEntry))
getConcurrentRetrocausalQueueEntry dp bid ak idx = do
  r <- guarded "get_concurrent_retrocausal_queue_entry" (\c -> call_get_concurrent_retrocausal_queue_entry c bid ak idx) dp
  bindSys r $ \case
    ExpectedQueueEntry_queue_entry x -> mapOptional KRPCNone (Just x)
    ExpectedQueueEntry_default e -> mapOptional e Nothing

commitTransaction :: DatabaseProxy -> BlockID -> TransactionID -> [Contract] -> IO (SystemStatus ())
commitTransaction dp bid tx contracts = do
  r <- guarded "commit_transaction" (\c -> call_commit_transaction c bid tx contracts) dp
  bindSys r mapVoid

abortTransaction :: DatabaseProxy -> BlockID -> TransactionID -> IO (SystemStatus ())
abortTransaction dp bid tx = do
  r <- guarded "abort_transaction" (\c -> call_abort_transaction c bid tx) dp
  bindSys r mapVoid

popSubtransactions :: DatabaseProxy -> BlockID -> TransactionID -> Word32 -> IO (SystemStatus ())
popSubtransactions dp bid tx n = do
  r <- guarded "pop_subtransactions" (\c -> call_pop_subtransactions c bid tx n) dp
  bindSys r mapVoid

-- | The tip the block at @height@ builds on.  A tip whose id equals the
-- block being loaded is refused.
dbGetDbTip :: DatabaseProxy -> BlockID -> BlockHeight -> IO (SystemStatus DbTip)
dbGetDbTip dp bid height = do
  r <- guarded "get_db_tip" (\c -> call_get_db_tip c bid height) dp
  bindSys r $ \reply -> do
    t <- case reply of
      ExpectedDBTip_db_tip t -> mapMandatory KRPCNone (Just t)
      ExpectedDBTip_default e -> mapMandatory e Nothing
    bindSys t $ \tip ->
      if tip.id == bid
        then do
          logError ("Unrecoverable: Received DbTip with block id equal to current block id: " ++ show bid ++ " (should be parent ID)")
          pure (Left SysUnrecoverable)
        else pure (Right tip)

openViewingSnapshot :: DatabaseProxy -> IO (SystemStatus ViewingSnapshot)
openViewingSnapshot dp = do
  r <- guarded "open_viewing_snapshot" call_open_viewing_snapshot dp
  bindSys r $ \case
    ExpectedViewingSnapshot_snapshot s -> mapMandatory KRPCNone (Just s)
    ExpectedViewingSnapshot_default e -> mapMandatory e Nothing

closeViewingSnapshot :: DatabaseProxy -> BlockID -> IO (SystemStatus ())
closeViewingSnapshot dp bid = do
  r <- guarded "close_viewing_snapshot" (\c -> call_close_viewing_snapshot c bid) dp
  bindSys r mapVoid

------------------------------------------------------------------------------
-- The subtransaction push size gate

-- | kExecToDbMessageCap: the budget the executor holds itself to on the HogDb
-- link (common/limits.h).
execToDbMessageCap :: Integer
execToDbMessageCap = 0x1000000

xdrWord :: Integer
xdrWord = 4

opaqueBound :: Int -> Integer
opaqueBound len = xdrWord + ((fromIntegral len + 3) `div` 4 * 4)

kRcqDeltaOverhead :: Integer
kRcqDeltaOverhead = xdrMax_RetrocausalQueuePastBlockConsumeDelta + 4 * xdrWord

-- | A conservative upper bound on the XDR size of one delta, from field
-- lengths only (database_proxy.cc footprint_bound).
footprintBound :: FlaggedStorageDelta -> Integer
footprintBound fd = perDeltaFraming + deltaBound fd.delta
  where
    perDeltaFraming = xdrMax_AddressAndKey + xdrMax_StorageDeltaFlagSet + xdrMax_StorageDeltaType
    deltaBound = \case
      StorageDelta_kDeltaTypeDelete -> 0
      StorageDelta_bytestring b -> opaqueBound (B.length b)
      StorageDelta_nnint _ -> xdrMax_NonnegativeInt128SetAddArgs
      StorageDelta_nnint_add _ -> xdrMax_X_int128_xdr
      StorageDelta_retrocausal_queue rq ->
        let entries =
              length rq.insert.added_entries
                + length rq.observed_concurrent_entries
                + maybe 0 (\c -> length c.current_block_consumed) rq.consume
         in kRcqDeltaOverhead + fromIntegral entries * xdrMax_QueueEntry
      StorageDelta_auth_data_change _ -> xdrMax_AuthDataChangeArgs
      StorageDelta_deployed_contract_hash _ -> xdrMax_DeployedContractHash
      StorageDelta_consume_uniquifier_args _ -> xdrMax_ConsumeUniquifierArgs

-- | The rpc_msg header, BlockID, TransactionID and the batch length prefix.
messageOverhead :: Integer
messageOverhead = xdrMax_X_rpc_msg + xdrMax_BlockID + xdrMax_TransactionID + xdrWord

-- | Push a delta batch as a new subtransaction.  Right False is a
-- concurrency-control conflict (including a batch too large for one message,
-- which is rejected before it is sent); the conflict diagnostic is returned
-- alongside.
tryPushNewSubtransaction
  :: DatabaseProxy -> BlockID -> TransactionID -> [FlaggedStorageDelta] -> IO (SystemStatus (Bool, DbConflictResult))
tryPushNewSubtransaction dp bid tx batch = do
  let footprint = sum (map footprintBound batch)
  if footprint + messageOverhead > execToDbMessageCap
    then do
      logDebug
        ( "rejecting subtransaction push as a conflict: delta-batch footprint " ++ show footprint
            ++ " + message overhead " ++ show messageOverhead ++ " exceeds the "
            ++ show execToDbMessageCap ++ "-byte executor->HogDb message cap"
        )
      pure (Right (False, DbConflictResult_kDbDiagnosticNone))
    else do
      r <- guarded "try_push_new_subtransaction" (\c -> call_try_push_new_subtransaction c bid tx batch) dp
      bindSys r $ \case
        TryPushSubtransactionResult_conflict conflict -> pure (Right (False, conflict))
        TryPushSubtransactionResult_default KRPCNone -> pure (Right (True, DbConflictResult_kDbDiagnosticNone))
        TryPushSubtransactionResult_default KRPCBadBlockID -> pure (Left SysAbandon)
        TryPushSubtransactionResult_default e -> do
          logError ("Unrecoverable: Received unhandled error type from RPC: " ++ show e)
          pure (Left SysUnrecoverable)

------------------------------------------------------------------------------
-- Per transaction

-- | A DatabaseProxy scoped to one transaction, staging the deltas of the next
-- subtransaction push.
data PerTransactionDatabaseProxy = PerTransactionDatabaseProxy
  { ptxProxy :: DatabaseProxy
  , ptxBlockId :: BlockID
  , ptxTxId :: TransactionID
  , ptxBatch :: IORef [FlaggedStorageDelta]
  -- ^ reversed
  , ptxConflict :: IORef DbConflictResult
  }

newPerTransactionDatabaseProxy :: DatabaseProxy -> BlockID -> TransactionID -> IO PerTransactionDatabaseProxy
newPerTransactionDatabaseProxy dp bid tx =
  PerTransactionDatabaseProxy dp bid tx <$> newIORef [] <*> newIORef DbConflictResult_kDbDiagnosticNone

ptxAddDelta :: PerTransactionDatabaseProxy -> AddressAndKey -> StorageDelta -> StorageDeltaFlagSet -> IO ()
ptxAddDelta p ak d flags =
  modifyIORef' (ptxBatch p) (FlaggedStorageDelta {addrkey = ak, delta = d, flags = flags} :)

-- | Conflict diagnostics from the most recent failed push; the None arm if the
-- last push succeeded.
ptxLastPushConflict :: PerTransactionDatabaseProxy -> IO DbConflictResult
ptxLastPushConflict p = readIORef (ptxConflict p)

ptxTryPushCurrentSubtransaction :: PerTransactionDatabaseProxy -> IO (SystemStatus Bool)
ptxTryPushCurrentSubtransaction p = do
  writeIORef (ptxConflict p) DbConflictResult_kDbDiagnosticNone
  batch <- reverse <$> readIORef (ptxBatch p)
  r <- tryPushNewSubtransaction (ptxProxy p) (ptxBlockId p) (ptxTxId p) batch
  writeIORef (ptxBatch p) []
  case r of
    Left e -> pure (Left e)
    Right (ok, conflict) -> do
      writeIORef (ptxConflict p) conflict
      pure (Right ok)

ptxPopSubtransactions :: PerTransactionDatabaseProxy -> Word32 -> IO (SystemStatus ())
ptxPopSubtransactions p = popSubtransactions (ptxProxy p) (ptxBlockId p) (ptxTxId p)

ptxCommitTransaction :: PerTransactionDatabaseProxy -> [Contract] -> IO (SystemStatus ())
ptxCommitTransaction p = commitTransaction (ptxProxy p) (ptxBlockId p) (ptxTxId p)

ptxAbortTransaction :: PerTransactionDatabaseProxy -> IO (SystemStatus ())
ptxAbortTransaction p = abortTransaction (ptxProxy p) (ptxBlockId p) (ptxTxId p)

_unusedFromMaybe :: Maybe Int -> Int
_unusedFromMaybe = fromMaybe 0
