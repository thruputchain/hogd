-- | An in-memory hog_db for executor tests: an ONC RPC server implementing
-- @DatabaseToExecutorInterfaceV1@ on localhost, so the executor reaches it
-- through its real 'DatabaseProxy' client, as the C++ tests reach a RAM
-- HogDb through HogdbWrapper.
--
-- Model:
--
-- * The /snapshot/ is what every read of the open block sees (items,
--   contracts, deployments).
-- * A transaction's pushed subtransactions stack up per (block, tx);
--   @pop_subtransactions@ drops the newest, @abort_transaction@ all of them,
--   and @commit_transaction@ applies them (plus the attached contracts) to
--   the block's /pending/ state.
-- * 'fdAdvanceBlock' folds the pending state into the snapshot and moves
--   the tip, so commits become visible to the next block (as they do in
--   hog_db).
--
-- Concurrency control is reduced to a range check: a push whose deltas
-- would leave an nnint or native token balance outside @0..INT128_MAX@
-- (over the pending state plus the transaction's earlier pushes) is refused
-- as a conflict.  Storage-cost accounting, retrocausal queues, auth-data
-- changes and replay-cache records are not modelled (their deltas are
-- accepted and dropped).
module Harness.FakeDb
  ( FakeDb
  , startFakeDb
  , stopFakeDb
  , fdPort
  , fdOpenBlockId
  , fdOpenHeight
    -- * Seeding and inspecting state
  , fdWriteItem
  , fdWriteContract
  , fdWriteDeployment
  , fdLookupItem
  , fdLookupPendingItem
  , fdLookupDeployment
  , fdLookupContract
  , fdAdvanceBlock
  , fdCalls
    -- * Delta application (exposed for tests)
  , applyDelta
  ) where

import Control.Concurrent.MVar
import qualified Data.ByteString as B
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Base.NetworkConfigParams (statedDefaults)
import Hogd.Exec.Common.Int128
import Hogd.Genesis.Configuration (makeDeployedContractHash, nativeDefaultWalletHash, sha256Xdr)
import Hogd.Xdr
import X23.Rpc
import X23.Xdr (xdrDecodePrefix)

data DbState = DbState
  { dsItems :: M.Map AddressAndKey WireStorageItem
  , dsContracts :: M.Map SHA256Hash Contract
  , dsDeployments :: M.Map ContractAddress DeployedContractHash
  , dsPendingItems :: M.Map AddressAndKey (Maybe WireStorageItem)
  -- ^ Nothing: deleted
  , dsPendingContracts :: M.Map SHA256Hash Contract
  , dsPendingDeployments :: M.Map ContractAddress DeployedContractHash
  , dsTxs :: M.Map (BlockID, TransactionID) [[FlaggedStorageDelta]]
  -- ^ pushed subtransactions, newest first
  , dsBlockId :: BlockID
  , dsHeight :: BlockHeight
  -- ^ the height of the open block (the tip's plus one)
  , dsCalls :: [String]
  -- ^ RPCs served, newest first
  }

data FakeDb = FakeDb
  { fdState :: MVar DbState
  , fdServer :: RpcServer
  }

fdPort :: FakeDb -> Word16
fdPort = rpcServerPort . fdServer

-- | Start serving, with block 1 (height 1) open over an empty ledger.
startFakeDb :: IO FakeDb
startFakeDb = do
  st <- newMVar (DbState M.empty M.empty M.empty M.empty M.empty M.empty M.empty 1 1 [])
  srv <- bindRpcServer "127.0.0.1" 0
  let db = FakeDb st srv
  startAccepting srv defaultMaxMessageSize [databaseToExecutorInterfaceV1_service (service db)]
  pure db

stopFakeDb :: FakeDb -> IO ()
stopFakeDb = stopRpcServer . fdServer

fdOpenBlockId :: FakeDb -> IO BlockID
fdOpenBlockId db = dsBlockId <$> readMVar (fdState db)

fdOpenHeight :: FakeDb -> IO BlockHeight
fdOpenHeight db = dsHeight <$> readMVar (fdState db)

-- | Names of the RPCs served so far, oldest first.
fdCalls :: FakeDb -> IO [String]
fdCalls db = reverse . dsCalls <$> readMVar (fdState db)

-- | Write an item straight into the snapshot (C++ write_item).
fdWriteItem :: FakeDb -> AddressAndKey -> WireStorageItem -> IO ()
fdWriteItem db ak item = modifyMVar_ (fdState db) $ \s -> pure s {dsItems = M.insert ak item (dsItems s)}

-- | Store a contract by its hash (C++ write_contract).
fdWriteContract :: FakeDb -> Contract -> IO SHA256Hash
fdWriteContract db c = do
  let h = sha256Xdr c
  modifyMVar_ (fdState db) $ \s -> pure s {dsContracts = M.insert h c (dsContracts s)}
  pure h

-- | Deploy a contract at an address (C++ write_deployment).
fdWriteDeployment :: FakeDb -> ContractAddress -> Contract -> IO ()
fdWriteDeployment db addr c = do
  _ <- fdWriteContract db c
  modifyMVar_ (fdState db) $ \s -> pure s {dsDeployments = M.insert addr (makeDeployedContractHash c) (dsDeployments s)}

-- | The snapshot's item (what the open block reads).
fdLookupItem :: FakeDb -> AddressAndKey -> IO (Maybe WireStorageItem)
fdLookupItem db ak = M.lookup ak . dsItems <$> readMVar (fdState db)

-- | The item as of the transactions committed so far in the open block.
fdLookupPendingItem :: FakeDb -> AddressAndKey -> IO (Maybe WireStorageItem)
fdLookupPendingItem db ak = do
  s <- readMVar (fdState db)
  pure (visible s ak)

fdLookupDeployment :: FakeDb -> ContractAddress -> IO (Maybe DeployedContractHash)
fdLookupDeployment db addr = M.lookup addr . dsDeployments <$> readMVar (fdState db)

fdLookupContract :: FakeDb -> SHA256Hash -> IO (Maybe Contract)
fdLookupContract db h = M.lookup h . dsContracts <$> readMVar (fdState db)

-- | Close the open block (making its commits the snapshot) and open the
-- next one.
fdAdvanceBlock :: FakeDb -> IO ()
fdAdvanceBlock db = modifyMVar_ (fdState db) $ \s ->
  pure
    s
      { dsItems = M.foldrWithKey (\k v m -> maybe (M.delete k m) (\i -> M.insert k i m) v) (dsItems s) (dsPendingItems s)
      , dsContracts = M.union (dsPendingContracts s) (dsContracts s)
      , dsDeployments = M.union (dsPendingDeployments s) (dsDeployments s)
      , dsPendingItems = M.empty
      , dsPendingContracts = M.empty
      , dsPendingDeployments = M.empty
      , dsTxs = M.empty
      , dsBlockId = dsBlockId s + 1
      , dsHeight = dsHeight s + 1
      }

visible :: DbState -> AddressAndKey -> Maybe WireStorageItem
visible s ak = case M.lookup ak (dsPendingItems s) of
  Just v -> v
  Nothing -> M.lookup ak (dsItems s)

-- | The effect of one delta on an item; Left for a value out of range.
applyDelta :: StorageDelta -> Maybe WireStorageItem -> Either String (Maybe WireStorageItem)
applyDelta d old = case d of
  StorageDelta_bytestring b -> Right (Just (WireStorageItem_bytestring b))
  StorageDelta_kDeltaTypeDelete -> Right Nothing
  StorageDelta_nnint a -> nn (fromXdr128 a.set_value + fromXdr128 a.add_value)
  StorageDelta_nnint_add a -> nn (current + fromXdr128 a)
  _ -> Right old
  where
    current = case old of
      Just (WireStorageItem_nnint v) -> fromXdr128 v
      _ -> 0
    nn v
      | v < 0 || v > int128Max = Left ("nnint out of range: " ++ show v)
      | otherwise = Right (Just (WireStorageItem_nnint (toXdr128 v)))

-- | Apply a batch over a view of the items; deployments are collected.
applyBatch
  :: (AddressAndKey -> Maybe WireStorageItem)
  -> [FlaggedStorageDelta]
  -> Either String (M.Map AddressAndKey (Maybe WireStorageItem), M.Map ContractAddress DeployedContractHash)
applyBatch base = go M.empty M.empty
  where
    go items deps [] = Right (items, deps)
    go items deps (fd : rest) = case fd.delta of
      StorageDelta_deployed_contract_hash dep -> go items (M.insert fd.addrkey.address dep deps) rest
      d
        | isModelled fd.addrkey.key d -> do
            let old = maybe (base fd.addrkey) id (M.lookup fd.addrkey items)
            new <- applyDelta d old
            go (M.insert fd.addrkey new items) deps rest
        | otherwise -> go items deps rest
    isModelled key d = case key of
      StorageKey_contract_data_key _ -> modelledDelta d
      StorageKey_FreeNativeTokenBalance -> modelledDelta d
      _ -> False
    modelledDelta = \case
      StorageDelta_bytestring _ -> True
      StorageDelta_kDeltaTypeDelete -> True
      StorageDelta_nnint _ -> True
      StorageDelta_nnint_add _ -> True
      _ -> False

record :: String -> DbState -> DbState
record name s = s {dsCalls = name : dsCalls s}

service :: FakeDb -> DatabaseToExecutorInterfaceV1
service db =
  DatabaseToExecutorInterfaceV1
    { try_push_new_subtransaction = \bid tx batch -> withState "try_push_new_subtransaction" $ \s ->
        let stack = M.findWithDefault [] (bid, tx) (dsTxs s)
            earlier = concat (reverse stack)
         in case applyBatch (visible s) (earlier ++ batch) of
              Left _ -> (s, TryPushSubtransactionResult_conflict DbConflictResult_kDbDiagnosticNone)
              Right _ -> (s {dsTxs = M.insert (bid, tx) (batch : stack) (dsTxs s)}, TryPushSubtransactionResult_default KRPCNone)
    , commit_transaction = \bid tx contracts -> withState "commit_transaction" $ \s ->
        let stack = M.findWithDefault [] (bid, tx) (dsTxs s)
         in case applyBatch (visible s) (concat (reverse stack)) of
              Left _ -> (s, KRPCStorageWriteFailed)
              Right (items, deps) ->
                ( s
                    { dsPendingItems = M.union items (dsPendingItems s)
                    , dsPendingDeployments = M.union deps (dsPendingDeployments s)
                    , dsPendingContracts = M.union (M.fromList [(sha256Xdr c, c) | c <- contracts]) (dsPendingContracts s)
                    , dsTxs = M.delete (bid, tx) (dsTxs s)
                    }
                , KRPCNone
                )
    , abort_transaction = \bid tx -> withState "abort_transaction" $ \s -> (s {dsTxs = M.delete (bid, tx) (dsTxs s)}, KRPCNone)
    , pop_subtransactions = \bid tx n -> withState "pop_subtransactions" $ \s ->
        let stack = M.findWithDefault [] (bid, tx) (dsTxs s)
         in (s {dsTxs = M.insert (bid, tx) (drop (fromIntegral n) stack) (dsTxs s)}, KRPCNone)
    , get_snapshot_item = \_ ak -> withState "get_snapshot_item" $ \s ->
        (s, maybe (ExpectedStorageItem_default KRPCNotFound) ExpectedStorageItem_item (M.lookup ak (dsItems s)))
    , get_next_retrocausal_queue_block = \_ _ _ -> withState "get_next_retrocausal_queue_block" $ \s ->
        (s, ExpectedRetrocausalQueueBlock_default KRPCNotFound)
    , get_concurrent_retrocausal_queue_entry = \_ _ _ -> withState "get_concurrent_retrocausal_queue_entry" $ \s ->
        (s, ExpectedQueueEntry_default KRPCNotFound)
    , get_contract = \_ h -> withState "get_contract" $ \s ->
        (s, maybe (ExpectedContract_default KRPCNotFound) ExpectedContract_contract (M.lookup h (dsContracts s)))
    , get_deployment = \_ addr -> withState "get_deployment" $ \s ->
        (s, maybe (ExpectedDeployment_default KRPCNotFound) ExpectedDeployment_deployed_hash (M.lookup addr (dsDeployments s)))
    , get_db_tip = \bid height -> withState "get_db_tip" $ \s -> (s, ExpectedDBTip_db_tip (makeTip (bid - 1) (height - 1)))
    , open_viewing_snapshot = withState "open_viewing_snapshot" $ \s ->
        (s, ExpectedViewingSnapshot_snapshot ViewingSnapshot {viewing_block_id = dsBlockId s, tip = makeTip (dsBlockId s - 1) (dsHeight s - 1)})
    , close_viewing_snapshot = \_ -> withState "close_viewing_snapshot" $ \s -> (s, KRPCNone)
    }
  where
    withState :: String -> (DbState -> (DbState, a)) -> IO a
    withState name f = modifyMVar (fdState db) (\s -> pure (f (record name s)))

-- | A tip at @height@: the stock network configuration and the native
-- default wallet.
makeTip :: BlockID -> BlockHeight -> DbTip
makeTip bid height =
  DbTip
    { id = bid
    , storage_long_hash = B.replicate 2048 0
    , value = consensusValueAt height
    , default_wallet = nativeDefaultWalletHash
    , network_config = statedDefaults
    }

-- | An all-zero consensus value with the given header height.
consensusValueAt :: BlockHeight -> ConsensusValue
consensusValueAt height =
  let ConsensusValue p e prev hdr txs ups = zeroValue
      ProposalHeader hext nh ch prop tp _ th sh eh = hdr
   in ConsensusValue p e prev (ProposalHeader hext nh ch prop tp height th sh eh) txs ups
  where
    zeroValue = case xdrDecodePrefix (B.replicate 65536 0) of
      Right (v, _) -> v
      Left err -> error ("consensusValueAt: " ++ err)
