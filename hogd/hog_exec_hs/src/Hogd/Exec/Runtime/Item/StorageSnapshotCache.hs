-- | Root cache for objects read from the ledger during one block
-- (runtime/item/storage_snapshot_cache.{h,cc}).  Shared between the block's
-- concurrent transactions; reset between blocks.
--
-- Lookups answer from the cache, or read the database without holding any
-- lock and then insert; if a concurrent load won the race, its value is kept
-- (both read the same snapshot).
module Hogd.Exec.Runtime.Item.StorageSnapshotCache
  ( ContractCacheEntry (..)
  , DeployedContract (..)
  , StorageSnapshotCache
  , newStorageSnapshotCache
  , sscDbProxy
  , sscBlockId
  , tryGetContractEntry
  , tryGetDeployedContract
  , tryGetSnapshotItem
  , loadDbTip
  , setDbTip
  , hasDbTip
  , getDbTip
  , getNetworkConfigParams
  , blockExecutionContext
  , setBlockExecutionContext
  , resetCache
  , setCacheBlockId
  ) where

import Data.IORef
import qualified Data.Map.Strict as M
import Hogd.Base.NetworkConfigParams (NetworkConfigParams, networkConfigParamsFromV0)
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Exec.Util (showByteArray)
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.SnapshotStorageItem
import Hogd.Exec.Runtime.PreparedContract
import Hogd.Exec.Runtime.Rv32.Contract (prepareStoredRv32)
import Hogd.Xdr.Contract
import Hogd.Xdr.NetworkConfig (NetworkConfig (..))
import Hogd.Xdr.RpcTypes
import Hogd.Xdr.Types

-- | A cached contract: the code plus its eagerly built image.
data ContractCacheEntry = ContractCacheEntry
  { cceContract :: !Contract
  , ccePrepared :: !PreparedContract
  }

-- | A deployed contract: its on-ledger hash metadata plus the cached entry,
-- which is Nothing for transaction-local deployments.
data DeployedContract = DeployedContract
  { dcHash :: !DeployedContractHash
  , dcEntry :: !(Maybe ContractCacheEntry)
  }

data Tip = Tip !DbTip !NetworkConfigParams

data StorageSnapshotCache = StorageSnapshotCache
  { sscDbProxy :: DatabaseProxy
  , sscBlockIdRef :: IORef BlockID
  , sscExecCtx :: IORef BlockExecutionContext
  , sscContracts :: IORef (M.Map SHA256Hash (Maybe ContractCacheEntry))
  , sscDeployments :: IORef (M.Map ContractAddress (Maybe DeployedContract))
  , sscItems :: IORef (M.Map AddressAndKey SnapshotStorageItem)
  , sscTip :: IORef (Maybe Tip)
  }

newStorageSnapshotCache :: BlockID -> BlockExecutionContext -> DatabaseProxy -> IO StorageSnapshotCache
newStorageSnapshotCache bid ctx db =
  StorageSnapshotCache db
    <$> newIORef bid
    <*> newIORef ctx
    <*> newIORef M.empty
    <*> newIORef M.empty
    <*> newIORef M.empty
    <*> newIORef Nothing

sscBlockId :: StorageSnapshotCache -> IO BlockID
sscBlockId = readIORef . sscBlockIdRef

-- | Insert unless present; return whichever value is cached.
insertIfAbsent :: Ord k => IORef (M.Map k v) -> k -> v -> IO v
insertIfAbsent ref k v = atomicModifyIORef' ref $ \m -> case M.lookup k m of
  Just existing -> (m, existing)
  Nothing -> (M.insert k v m, v)

-- | The cached contract for @hash@, or Nothing for a negative lookup.
tryGetContractEntry :: StorageSnapshotCache -> SHA256Hash -> IO (SystemStatus (Maybe ContractCacheEntry))
tryGetContractEntry c hash = do
  cached <- M.lookup hash <$> readIORef (sscContracts c)
  case cached of
    Just e -> pure (Right e)
    Nothing -> loadContractEntry c hash

loadContractEntry :: StorageSnapshotCache -> SHA256Hash -> IO (SystemStatus (Maybe ContractCacheEntry))
loadContractEntry c hash = do
  bid <- sscBlockId c
  r <- getContract (sscDbProxy c) bid hash
  case r of
    Left e -> pure (Left e)
    Right mcontract -> do
      entry <- case mcontract of
        Nothing -> pure (Right Nothing)
        Just contract -> case contract of
          Contract_rv32_contract blob ->
            -- Deployed under whatever bounds were in force then; the
            -- ceilings keep it runnable after the chain lowers them.  A stored
            -- contract that no longer translates is a node fault.
            case prepareStoredRv32 blob of
              Left _ -> do
                logError ("Unrecoverable: stored rv32 contract " ++ showByteArray hash ++ " does not translate")
                pure (Left SysUnrecoverable)
              Right prepared ->
                pure (Right (Just (ContractCacheEntry contract (PreparedContract (Just prepared)))))
          _ -> pure (Right (Just (ContractCacheEntry contract emptyPreparedContract)))
      case entry of
        Left e -> pure (Left e)
        Right v -> Right <$> insertIfAbsent (sscContracts c) hash v

-- | The deployment at @addr@ in the snapshot, with its code warmed.
tryGetDeployedContract :: StorageSnapshotCache -> ContractAddress -> IO (SystemStatus (Maybe DeployedContract))
tryGetDeployedContract c addr = do
  cached <- M.lookup addr <$> readIORef (sscDeployments c)
  case cached of
    Just d -> pure (Right d)
    Nothing -> do
      bid <- sscBlockId c
      r <- getDeployment (sscDbProxy c) bid addr
      case r of
        Left e -> pure (Left e)
        Right Nothing -> Right <$> insertIfAbsent (sscDeployments c) addr Nothing
        Right (Just meta) -> do
          entry <- tryGetContractEntry c meta.hash
          case entry of
            Left e -> pure (Left e)
            Right ent -> Right <$> insertIfAbsent (sscDeployments c) addr (Just (DeployedContract meta ent))

-- | The snapshot item under a key; absent items are cached as deleted.
tryGetSnapshotItem :: StorageSnapshotCache -> AddressAndKey -> IO (SystemStatus SnapshotStorageItem)
tryGetSnapshotItem c ak = do
  cached <- M.lookup ak <$> readIORef (sscItems c)
  case cached of
    Just i -> pure (Right i)
    Nothing -> do
      bid <- sscBlockId c
      r <- getSnapshotItem (sscDbProxy c) bid ak
      case r of
        Left e -> pure (Left e)
        Right item -> do
          entry <- makeSnapshotEntry item ak.key
          case entry of
            Left e -> pure (Left e)
            Right en -> do
              i <- newSnapshotStorageItem en ak bid (sscDbProxy c)
              Right <$> insertIfAbsent (sscItems c) ak i

-- | Fetch and pin the block's db tip.  Unrecoverable if the tip disagrees
-- with the height the block was opened with.
loadDbTip :: StorageSnapshotCache -> IO (SystemStatus ())
loadDbTip c = do
  bid <- sscBlockId c
  ctx <- readIORef (sscExecCtx c)
  r <- getDbTip (sscDbProxy c) bid ctx.block_height
  case r of
    Left e -> pure (Left e)
    Right tip -> do
      let derived = tip.value.proposal_header.block_height + 1
      if ctx.block_height /= derived
        then do
          logError ("Block height mismatch: opened with " ++ show ctx.block_height ++ ", db tip implies " ++ show derived)
          pure (Left SysUnrecoverable)
        else Right <$> setDbTip c tip

-- | Pin a tip the caller already holds.
setDbTip :: StorageSnapshotCache -> DbTip -> IO ()
setDbTip c tip = do
  existing <- readIORef (sscTip c)
  checkM (null existing) "db tip already set"
  ctx <- readIORef (sscExecCtx c)
  checkM (ctx.block_height == tip.value.proposal_header.block_height + 1) "db tip height mismatch"
  params <- case tip.network_config of
    NetworkConfig_v0 v0 -> pure (networkConfigParamsFromV0 v0)
  writeIORef (sscTip c) (Just (Tip tip params))

hasDbTip :: StorageSnapshotCache -> IO Bool
hasDbTip c = maybe False (const True) <$> readIORef (sscTip c)

getDbTip :: StorageSnapshotCache -> IO DbTip
getDbTip c =
  readIORef (sscTip c) >>= \case
    Just (Tip t _) -> pure t
    Nothing -> panic "CHECK failed: db tip not loaded"

getNetworkConfigParams :: StorageSnapshotCache -> IO NetworkConfigParams
getNetworkConfigParams c =
  readIORef (sscTip c) >>= \case
    Just (Tip _ p) -> pure p
    Nothing -> panic "CHECK failed: network config params not loaded"

-- | The executing block's own facts (not derivable from the parent tip).
blockExecutionContext :: StorageSnapshotCache -> IO BlockExecutionContext
blockExecutionContext = readIORef . sscExecCtx

setBlockExecutionContext :: StorageSnapshotCache -> BlockExecutionContext -> IO ()
setBlockExecutionContext c = writeIORef (sscExecCtx c)

-- | Re-arm for reuse: clear every cache and poison the execution context.
resetCache :: StorageSnapshotCache -> IO ()
resetCache c = do
  writeIORef (sscContracts c) M.empty
  writeIORef (sscDeployments c) M.empty
  writeIORef (sscItems c) M.empty
  writeIORef (sscBlockIdRef c) maxBound
  writeIORef (sscExecCtx c) BlockExecutionContext {block_height = minBound, block_timepoint = 0}
  writeIORef (sscTip c) Nothing

setCacheBlockId :: StorageSnapshotCache -> BlockID -> IO ()
setCacheBlockId c bid = do
  writeIORef (sscBlockIdRef c) bid
  writeIORef (sscTip c) Nothing


