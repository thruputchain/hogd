-- | The executor's client for the event server
-- (runtime/event_server_proxy.{h,cc}).
module Hogd.Exec.Runtime.EventServerProxy
  ( EventServerProxy
  , connectEventServer
  , eventServerFromCard
  , eventOpenBlock
  , eventCloseBlock
  , emitEvent
  , reportTxFailure
  , PerTransactionEventServerProxy
  , newPerTransactionEventServerProxy
  , ptxEmitEvents
  , ptxReportTxFailure
  ) where

import Control.Concurrent (forkIO)
import Control.Exception (SomeException, try)
import Control.Monad (void, when)
import qualified Data.ByteString.Char8 as BC
import Data.IORef
import Data.Word (Word16)
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Xdr
import X23.Rpc

newtype EventServerProxy = EventServerProxy RpcClient

-- | Connect, retrying while the server is not yet accepting.
connectEventServer :: String -> Word16 -> Int -> IO EventServerProxy
connectEventServer host port maxMsg = do
  logInfo ("Connecting to event server on host port " ++ host ++ ":" ++ show port)
  EventServerProxy <$> connectRpcClientWith True maxMsg host port

eventServerFromCard :: EventCard -> IO EventServerProxy
eventServerFromCard card = do
  logInfo ("Connecting to event server at " ++ show card)
  case parseHostPort (BC.unpack card.address_for_exec) of
    Nothing -> panic "Failed to parse event server address"
    Just (h, p) -> connectEventServer h p defaultMaxMessageSize

mapResult :: String -> RPCError -> IO (SystemStatus ())
mapResult what r = do
  when (r /= KRPCNone) $ logError (what ++ " failed: " ++ show r)
  case r of
    KRPCNone -> pure (Right ())
    KRPCBadBlockID -> pure (Left SysAbandon)
    _ -> do
      logError ("Unrecoverable: Received unhandled error type from event server: " ++ show r)
      pure (Left SysUnrecoverable)

callGuarded :: String -> IO RPCError -> IO (SystemStatus ())
callGuarded what act = do
  r <- try act
  case r of
    Left (e :: SomeException) -> do
      logError ("Unrecoverable: " ++ what ++ " threw before an RPC status: " ++ show e)
      pure (Left SysUnrecoverable)
    Right code -> mapResult what code

eventOpenBlock :: EventServerProxy -> BlockID -> IO (SystemStatus ())
eventOpenBlock (EventServerProxy c) bid = callGuarded "open_block" (call_eventexecutor_open_block c bid)

eventCloseBlock :: EventServerProxy -> BlockID -> IO (SystemStatus ())
eventCloseBlock (EventServerProxy c) bid = callGuarded "close_block" (call_eventexecutor_close_block c bid)

emitEvent :: EventServerProxy -> BlockID -> TransactionID -> TxEventLog -> ProposedTransaction -> IO (SystemStatus ())
emitEvent (EventServerProxy c) bid tx logv ptx =
  callGuarded "emit_event" (call_eventexecutor_emit_event c bid tx logv ptx)

-- | Best-effort, detached report that a transaction was excluded from the
-- proposal.  Never fails.
reportTxFailure :: EventServerProxy -> TransactionHash -> TxFailureDiagnostic -> IO ()
reportTxFailure (EventServerProxy c) h diag = void $ forkIO $ do
  r <- try (call_eventexecutor_report_tx_failure c h diag)
  case r of
    Left (e :: SomeException) -> logWarn ("tx-failure report send failed: " ++ show e)
    Right KRPCNone -> pure ()
    Right code -> logWarn ("tx-failure report dropped by event server: " ++ show code)

data PerTransactionEventServerProxy = PerTransactionEventServerProxy
  { pteProxy :: EventServerProxy
  , pteBlock :: BlockID
  , pteTx :: TransactionID
  , pteEmitted :: IORef Bool
  }

newPerTransactionEventServerProxy :: EventServerProxy -> BlockID -> TransactionID -> IO PerTransactionEventServerProxy
newPerTransactionEventServerProxy p bid tx = PerTransactionEventServerProxy p bid tx <$> newIORef False

-- | Emit a transaction's event log; only once per transaction.
ptxEmitEvents :: PerTransactionEventServerProxy -> TxEventLog -> ProposedTransaction -> IO (SystemStatus ())
ptxEmitEvents p logv ptx = do
  already <- atomicModifyIORef' (pteEmitted p) (True,)
  if already
    then pure (Left SysUnrecoverable)
    else emitEvent (pteProxy p) (pteBlock p) (pteTx p) logv ptx

ptxReportTxFailure :: PerTransactionEventServerProxy -> TransactionHash -> TxFailureDiagnostic -> IO ()
ptxReportTxFailure p = reportTxFailure (pteProxy p)
