-- | A test environment for driving transactions through the executor
-- against an in-memory ledger, after the C++ NativeContractTestFramework
-- (runtime/native_contract_test_framework.h).
--
-- 'withTestEnv' starts a 'FakeDb' and a 'FakeEventServer', connects the
-- executor's real 'DatabaseProxy' and 'EventServerProxy' clients to them,
-- seeds the genesis execution prerequisites the executor needs (the native
-- token's code and deployment and the native default wallet's code) and
-- opens block 1.  Then:
--
-- * seed state with 'writeContract', 'deployContract', 'writeItem',
--   'grantNativeTokens';
-- * run a transaction with 'runTx' (root invoke, commit, event emission),
--   or build a handler with 'newHandler' to drive it by hand;
-- * 'advanceBlock' to make the block's commits visible, then inspect state
--   with 'lookupItem', 'nativeBalance', 'lookupDeployment'.
--
-- Runtime contexts: 'precompilesContext' is the production native context
-- (native token, default wallet); 'nativeTestContext' adds test-only native
-- contracts under arbitrary ids, as the C++ tests build NativeContexts.
module Harness.Env
  ( TestEnv (..)
  , withTestEnv
    -- * Blocks and transactions
  , advanceBlock
  , discardBlock
  , PendingState (..)
  , currentBlockId
  , newUniquifier
  , newHandler
  , newHandlerWith
  , newValidationHandler
  , TxRun (..)
  , runTx
  , runTxWith
  , rootTransaction
  , rootTransactionWithClaim
  , sourceAddressClaim
  , pubkeyClaim
    -- * Contexts
  , precompilesContext
  , nativeTestContext
  , nativeOnlyContext
    -- * State
  , writeContract
  , deployContract
  , writeItem
  , lookupItem
  , lookupPendingItem
  , lookupDeployment
  , grantNativeTokens
  , nativeBalance
  , contractDataKey
  , nativeBalanceKey
    -- * Helpers
  , contractAddress
  , nativeContractXdr
  , kTestBlockTimepoint
  ) where

import Control.Exception (bracket)
import Data.IORef
import Data.Int (Int32)
import qualified Data.Map.Strict as M
import Data.Word
import qualified Data.ByteString as B
import Harness.FakeDb
import Harness.FakeEventServer
import Hogd.Base.NetworkConfigParams (kDefaultMaxTxGas)
import Hogd.Exec.Common.Int128 (fromXdr128, toXdr128)
import Hogd.Exec.Common.Status (InvokeResult (..))
import Hogd.Exec.Components.ExecutionMode (ExecutionModeCtor (..))
import Hogd.Exec.Precompiles.Context (hogExecPrecompiles)
import Hogd.Exec.Runtime.EventServerProxy
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.StorageSnapshotCache
import Hogd.Exec.Runtime.NativeRuntime (NativeContract, multiplexedContext, nativeContext)
import Hogd.Exec.Runtime.SyscallHandler hiding (deployContract)
import Hogd.Exec.Runtime.Types (MethodID, RuntimeContext, SystemCallHandler)
import Hogd.Genesis.Configuration (makeNativeContract)
import Hogd.Genesis.Constants (kNativeTokenContractAddress)
import Hogd.Xdr
import X23.Rpc (defaultMaxMessageSize)

data TestEnv = TestEnv
  { teDb :: FakeDb
  , teEvents :: FakeEventServer
  , teDbProxy :: DatabaseProxy
  , teEventProxy :: EventServerProxy
  , teCache :: IORef StorageSnapshotCache
  , teNextTx :: IORef TransactionID
  , teNextNonce :: IORef Word64
  }

-- | The block timepoint every test block opens with.
kTestBlockTimepoint :: TimePoint
kTestBlockTimepoint = 1700000000

withTestEnv :: (TestEnv -> IO a) -> IO a
withTestEnv act =
  bracket startFakeDb stopFakeDb $ \db ->
    bracket startFakeEventServer stopFakeEventServer $ \ev -> do
      proxy <- newDatabaseProxy (LocalMockDB (fdPort db)) defaultMaxMessageSize
      evProxy <- connectEventServer "127.0.0.1" (fesPort ev) defaultMaxMessageSize
      -- Genesis execution prerequisites (less the genesis root): the native
      -- token's code and deployment, the native default wallet's code.
      fdWriteDeployment db kNativeTokenContractAddress (makeNativeContract KNativeContractNativeToken)
      _ <- fdWriteContract db (makeNativeContract KNativeContractDefaultWallet)
      cache <- openCache db proxy
      env <- TestEnv db ev proxy evProxy <$> newIORef cache <*> newIORef 0 <*> newIORef 0
      act env

openCache :: FakeDb -> DatabaseProxy -> IO StorageSnapshotCache
openCache db proxy = do
  bid <- fdOpenBlockId db
  height <- fdOpenHeight db
  newStorageSnapshotCache bid BlockExecutionContext {block_height = height, block_timepoint = kTestBlockTimepoint} proxy

-- | Commit the open block and open the next one, with a fresh snapshot
-- cache.
advanceBlock :: TestEnv -> IO ()
advanceBlock env = do
  fdAdvanceBlock (teDb env)
  cache <- openCache (teDb env) (teDbProxy env)
  writeIORef (teCache env) cache

-- | Abandon the open block (C++ discard_block): reopen one at the same
-- height over the same snapshot, so the block can be replayed (e.g. in
-- validation mode).  Returns what the abandoned block had committed.
discardBlock :: TestEnv -> IO PendingState
discardBlock env = do
  st <- fdDiscardBlock (teDb env)
  cache <- openCache (teDb env) (teDbProxy env)
  writeIORef (teCache env) cache
  pure st

currentBlockId :: TestEnv -> IO BlockID
currentBlockId = fdOpenBlockId . teDb

-- | A fresh, never-expiring uniquifier (C++ make_new_uniquifier).
newUniquifier :: TestEnv -> IO Uniquifier
newUniquifier env = do
  n <- atomicModifyIORef' (teNextNonce env) (\n -> (n + 1, n))
  pure Uniquifier {block_expiry_height = maxBound, nonce = n}

-- | A proposal-mode handler for a transaction (C++
-- make_new_system_call_handler), with its transaction id.
newHandler :: TestEnv -> RuntimeContext -> Transaction -> IO (SystemCallHandler, TransactionID)
newHandler env rc = newHandlerWith env rc ProposalModeCtor

-- | A validation-mode handler replaying the given proposal data (C++
-- make_new_system_call_handler_validation).
newValidationHandler :: TestEnv -> RuntimeContext -> NondeterministicProposalData -> Transaction -> IO (SystemCallHandler, TransactionID)
newValidationHandler env rc d = newHandlerWith env rc (ValidationModeCtor d)

newHandlerWith :: TestEnv -> RuntimeContext -> ExecutionModeCtor -> Transaction -> IO (SystemCallHandler, TransactionID)
newHandlerWith env rc mode tx = do
  cache <- readIORef (teCache env)
  loaded <- hasDbTip cache
  if loaded
    then pure ()
    else loadDbTip cache >>= either (\e -> fail ("loading the db tip failed: " ++ show e)) pure
  bid <- currentBlockId env
  txid <- atomicModifyIORef' (teNextTx env) (\n -> (n + 1, n))
  db <- newPerTransactionDatabaseProxy (teDbProxy env) bid txid
  ev <- newPerTransactionEventServerProxy (teEventProxy env) bid txid
  h <- newSystemCallHandler rc cache db ev mode tx
  pure (h, txid)

-- | Everything a test usually checks about one transaction.
data TxRun = TxRun
  { trRoot :: InvokeResult
  , trOutput :: InvocationFrameOutput
  -- ^ the root frame's output, read before commit
  , trCommit :: ExecResult
  , trEventLog :: Maybe TxEventLog
  -- ^ what reached the event server (included transactions only)
  , trTxId :: TransactionID
  }
  deriving (Show)

-- | Root invoke, commit, and (for an included transaction) emit events.
runTx :: TestEnv -> RuntimeContext -> Transaction -> IO TxRun
runTx env rc tx = runTxWith env rc tx (\_ -> pure ())

-- | 'runTx' with a hook on the handler after the root invocation and
-- before commit.
runTxWith :: TestEnv -> RuntimeContext -> Transaction -> (SystemCallHandler -> IO ()) -> IO TxRun
runTxWith env rc tx hook = do
  (h, txid) <- newHandler env rc tx
  root <- rootInvoke h
  output <- getMostRecentFrameOutput h
  hook h
  commit <- tryCommitTransaction h
  logv <-
    if commit `elem` [ExecResultOK, ExecResultTxRootFrameReverted, ExecResultTxDeterministicError]
      then do
        let ptx =
              ProposedTransaction
                { signed_tx = SignedTransaction {tx = tx, signatures = [], pubkey_claim_key = Nothing}
                , nondeterministic_proposal_data =
                    NondeterministicProposalData {nondeterministic_bits = B.empty, nondeterministic_bit_count = 0, observed_queue_entry_sequence = []}
                }
        r <- emitEvents h commit ptx
        either (\e -> fail ("emit_events failed: " ++ show e)) pure r
        bid <- currentBlockId env
        fesEventLog (teEvents env) bid txid
      else pure Nothing
  pure (TxRun root output commit logv txid)

-- | @AuthClaim{}@: a source-address claim with no keys.
sourceAddressClaim :: AuthClaim
sourceAddressClaim = AuthClaim_source_address_claim AuthClaim_source_address_claim_t {allowed_keys = 0, required_weight = 0}

pubkeyClaim :: PubkeyAccountPreimage -> AuthClaim
pubkeyClaim = AuthClaim_pubkey_preimage

-- | A transaction root-invoking @method@ on @addr@ with the default
-- (source-address) auth claim and the chain's max gas.
rootTransaction :: TestEnv -> ContractAddress -> MethodID -> Calldata -> IO Transaction
rootTransaction env addr method calldata = rootTransactionWithClaim env addr method calldata sourceAddressClaim

rootTransactionWithClaim :: TestEnv -> ContractAddress -> MethodID -> Calldata -> AuthClaim -> IO Transaction
rootTransactionWithClaim env addr method calldata claim = do
  u <- newUniquifier env
  pure
    Transaction
      { invocation =
          Invocation
            { invoked_address = addr
            , entrypoint = entrypointFromMethodId method
            , gas_limit = kDefaultMaxTxGas
            , calldata = calldata
            }
      , uniquifier = u
      , contracts_to_deploy = []
      , auth_claim = claim
      }

-- | The production native context: the native token and the default wallet.
precompilesContext :: RuntimeContext
precompilesContext = nativeTestContext []

-- | The precompiles plus test native contracts under the given ids, in a
-- multiplexed context (native only).
nativeTestContext :: [(Int32, NativeContract)] -> RuntimeContext
nativeTestContext extra = multiplexedContext (M.singleton KNative (nativeOnlyContext extra))

-- | The bare NativeContext of 'nativeTestContext' (to multiplex with
-- others).
nativeOnlyContext :: [(Int32, NativeContract)] -> RuntimeContext
nativeOnlyContext extra =
  nativeContext (M.union (M.fromList [(HogexecPrecompiledNativeContracts i, c) | (i, c) <- extra]) hogExecPrecompiles)

-- | A native contract record (C++ make_native_contract).
nativeContractXdr :: Int32 -> Contract
nativeContractXdr = Contract_native_contract_id . HogexecPrecompiledNativeContracts

-- | @make_contract_address(id)@: zero but for the first byte.
contractAddress :: Word8 -> ContractAddress
contractAddress i = B.cons i (B.replicate 31 0)

writeContract :: TestEnv -> Contract -> IO SHA256Hash
writeContract env = fdWriteContract (teDb env)

-- | Store a contract and deploy it at an address.
deployContract :: TestEnv -> ContractAddress -> Contract -> IO ()
deployContract env = fdWriteDeployment (teDb env)

writeItem :: TestEnv -> AddressAndKey -> WireStorageItem -> IO ()
writeItem env = fdWriteItem (teDb env)

-- | The item as the open block's snapshot holds it.
lookupItem :: TestEnv -> AddressAndKey -> IO (Maybe WireStorageItem)
lookupItem env = fdLookupItem (teDb env)

-- | The item after the open block's commits so far.
lookupPendingItem :: TestEnv -> AddressAndKey -> IO (Maybe WireStorageItem)
lookupPendingItem env = fdLookupPendingItem (teDb env)

lookupDeployment :: TestEnv -> ContractAddress -> IO (Maybe DeployedContractHash)
lookupDeployment env = fdLookupDeployment (teDb env)

contractDataKey :: ContractAddress -> StorageKeyContractData -> AddressAndKey
contractDataKey addr k = AddressAndKey {address = addr, key = StorageKey_contract_data_key k}

nativeBalanceKey :: ContractAddress -> AddressAndKey
nativeBalanceKey addr = AddressAndKey {address = addr, key = StorageKey_FreeNativeTokenBalance}

-- | Set an address's committed native token balance.
grantNativeTokens :: TestEnv -> ContractAddress -> Integer -> IO ()
grantNativeTokens env addr amount = writeItem env (nativeBalanceKey addr) (WireStorageItem_nnint (toXdr128 amount))

-- | An address's native token balance in the open block's snapshot.
nativeBalance :: TestEnv -> ContractAddress -> IO Integer
nativeBalance env addr = do
  item <- lookupItem env (nativeBalanceKey addr)
  pure $ case item of
    Just (WireStorageItem_nnint v) -> fromXdr128 v
    _ -> 0
