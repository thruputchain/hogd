-- | A storage item as it exists in the block's snapshot state, with lazy,
-- shared access to retrocausal-queue data (runtime/item/snapshot_storage_item.*).
--
-- Items are shared by every concurrent transaction of a block.  The lazily
-- loaded queue state is guarded by an MVar held across database reads, as the
-- C++ CoroLock is.
module Hogd.Exec.Runtime.Item.SnapshotStorageItem
  ( ItemViewType (..)
  , SnapshotEntry (..)
  , SnapshotStorageItem
  , ssiAddrKey
  , ssiEntry
  , makeSnapshotEntry
  , newSnapshotStorageItem
  , snapshotItemType
  , getNextRcqBlock
  , getConcurrentRcqEntry
  , getRetrocausallyConsumedEntries
  ) where

import Control.Concurrent.MVar
import Data.Foldable (toList)
import qualified Data.Map.Strict as M
import Data.Sequence (Seq, (|>))
import qualified Data.Sequence as Seq
import Data.Word
import Hogd.Exec.Common.Int128
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Xdr.AddressAuth (AddressAuthData)
import Hogd.Xdr.RetrocausalQueue
import Hogd.Xdr.RpcTypes (BlockID)
import Hogd.Xdr.StorageItem
import Hogd.Xdr.Types

-- | The type of an item as viewed by a contract (item_view_type.h).  This may
-- differ from the item's logical type in the database: kNNInt and
-- kNativeTokenBalance are both nnints there, told apart by the key type.
data ItemViewType
  = IVBytestring
  | IVNNInt
  | IVRetrocausalQueue
  | IVNativeTokenBalance
  | IVReplayCache
  | IVAddressAuthData
  deriving (Eq, Show)

data SnapshotEntry
  = SEBytestring !Bytestring
  | SENNInt !Integer
  | SERetrocausalQueue !RetrocausalQueueMetadata
  | SEDeleted
  -- ^ cached negative lookups, so a queue deleted and recreated this block
  -- still has its concurrent entries available
  | SENativeTokenBalance !Integer
  | SEReplayCache
  | SEAddressAuthData !AddressAuthData
  deriving (Show)

data RcqState = RcqState
  { rsBlocks :: !(M.Map Word64 RetrocausalQueueBlock)
  , rsUpperBound :: !(Maybe Word64)
  -- ^ no need to query for blocks at or above this number
  , rsConcurrent :: !(Seq QueueEntry)
  -- ^ concurrent entries loaded so far, in index order
  }

data SnapshotStorageItem = SnapshotStorageItem
  { ssiAddrKey :: !AddressAndKey
  , ssiEntry :: !SnapshotEntry
  , ssiBlockId :: !BlockID
  , ssiDb :: DatabaseProxy
  , ssiRcq :: MVar RcqState
  }

newSnapshotStorageItem :: SnapshotEntry -> AddressAndKey -> BlockID -> DatabaseProxy -> IO SnapshotStorageItem
newSnapshotStorageItem e ak bid db =
  SnapshotStorageItem ak e bid db <$> newMVar (RcqState M.empty Nothing Seq.empty)

snapshotItemType :: SnapshotStorageItem -> Maybe ItemViewType
snapshotItemType s = case ssiEntry s of
  SEBytestring _ -> Just IVBytestring
  SENNInt _ -> Just IVNNInt
  SERetrocausalQueue _ -> Just IVRetrocausalQueue
  SEDeleted -> Nothing
  SENativeTokenBalance _ -> Just IVNativeTokenBalance
  SEReplayCache -> Just IVReplayCache
  SEAddressAuthData _ -> Just IVAddressAuthData

-- | Map a wire item (Nothing = absent / deleted) under a key to its entry.
-- Unrecoverable for an unexpected (key type, item type) combination.
makeSnapshotEntry :: Maybe WireStorageItem -> StorageKey -> IO (SystemStatus SnapshotEntry)
makeSnapshotEntry item key = case key of
  StorageKey_contract_data_key _ -> case item of
    Just (WireStorageItem_bytestring b) -> ok (SEBytestring b)
    Just (WireStorageItem_nnint n) -> ok (SENNInt (fromXdr128 n))
    Just (WireStorageItem_metadata m) -> ok (SERetrocausalQueue m)
    Nothing -> ok SEDeleted
    Just WireStorageItem_kItemTypeReplayCacheData -> ok SEReplayCache
    Just other -> bad ("unsupported wire item type " ++ show (itemTag other) ++ " for ContractData key")
  StorageKey_FreeNativeTokenBalance -> case item of
    Just (WireStorageItem_nnint n) -> ok (SENativeTokenBalance (fromXdr128 n))
    Nothing -> ok (SENativeTokenBalance 0)
    Just other -> bad ("unsupported wire item type " ++ show (itemTag other) ++ " for FreeNativeTokenBalance key")
  StorageKey_AuthData -> case item of
    Just (WireStorageItem_auth_data a) -> ok (SEAddressAuthData a)
    Nothing -> ok SEDeleted
    Just other -> bad ("unsupported wire item type " ++ show (itemTag other) ++ " for AuthData key")
  _ -> bad ("unsupported storage key type " ++ show key)
  where
    ok = pure . Right
    bad msg = do
      logError ("Unrecoverable: " ++ msg)
      pure (Left SysUnrecoverable)
    itemTag = discriminant_WireStorageItem

-- | The first past block at or after @start@ that still holds entries, or
-- Nothing.  Only RCQ items have blocks.
getNextRcqBlock :: SnapshotStorageItem -> Word64 -> IO (SystemStatus (Maybe RetrocausalQueueBlock))
getNextRcqBlock s start = case ssiEntry s of
  SERetrocausalQueue _ -> modifyMVar (ssiRcq s) $ \st ->
    case rsUpperBound st of
      Just ub | start >= ub -> pure (st, Right Nothing)
      _ -> case M.lookupGE start (rsBlocks st) of
        Just (_, blk) -> pure (st, Right (Just blk))
        Nothing -> do
          r <- getNextRetrocausalQueueBlock (ssiDb s) (ssiBlockId s) (ssiAddrKey s) start
          case r of
            Left e -> pure (st, Left e)
            Right Nothing -> pure (st {rsUpperBound = Just start}, Right Nothing)
            Right (Just blk) ->
              pure (st {rsBlocks = M.insert blk.block_number blk (rsBlocks st)}, Right (Just blk))
  _ -> pure (Right Nothing)

-- | The concurrent (this-block) entry at @index@.  Entries must be loaded in
-- index order; a racing load of the same index must see the same answer.
getConcurrentRcqEntry :: SnapshotStorageItem -> Word32 -> IO (SystemStatus (Maybe QueueEntry))
getConcurrentRcqEntry s index = do
  cached <- withMVar (ssiRcq s) $ \st ->
    let i = fromIntegral index
     in if i < Seq.length (rsConcurrent st)
          then pure (Right (Just (Seq.index (rsConcurrent st) i)))
          else
            if i /= Seq.length (rsConcurrent st)
              then do
                logError "unrecoverable: bad get_concurrent_rcq_entry access"
                pure (Left SysUnrecoverable)
              else pure (Right Nothing)
  case cached of
    Left e -> pure (Left e)
    Right (Just e) -> pure (Right (Just e))
    Right Nothing -> do
      r <- getConcurrentRetrocausalQueueEntry (ssiDb s) (ssiBlockId s) (ssiAddrKey s) index
      case r of
        Left e -> pure (Left e)
        Right Nothing -> pure (Right Nothing)
        Right (Just entry) -> do
          recorded <- modifyMVar (ssiRcq s) $ \st ->
            let i = fromIntegral index
             in if i == Seq.length (rsConcurrent st)
                  then pure (st {rsConcurrent = rsConcurrent st |> entry}, Right ())
                  else
                    if Seq.index (rsConcurrent st) i /= entry
                      then do
                        logError "unrecoverable: mismatch in concurrent rcq return results"
                        pure (st, Left SysUnrecoverable)
                      else pure (st, Right ())
          pure (fmap (const (Just entry)) recorded)

-- | The first @count@ concurrent entries; they must already be loaded.
getRetrocausallyConsumedEntries :: SnapshotStorageItem -> Word32 -> IO [QueueEntry]
getRetrocausallyConsumedEntries s count = withMVar (ssiRcq s) $ \st -> do
  checkM (fromIntegral count <= Seq.length (rsConcurrent st)) "consumed entries not loaded"
  pure (toList (Seq.take (fromIntegral count) (rsConcurrent st)))
