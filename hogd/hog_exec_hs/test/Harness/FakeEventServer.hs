-- | An in-memory event server for executor tests: an ONC RPC server
-- implementing the executor-facing @EventExecutor1@ interface that records
-- every emitted transaction event log and failure report (C++
-- runtime/test_util/test_event_aggregator.*).
module Harness.FakeEventServer
  ( FakeEventServer
  , startFakeEventServer
  , stopFakeEventServer
  , fesPort
  , fesEventLogs
  , fesEventLog
  , fesFailureReports
  ) where

import Control.Concurrent.MVar
import Data.Word
import Hogd.Xdr
import X23.Rpc

data FakeEventServer = FakeEventServer
  { fesLogs :: MVar [(BlockID, TransactionID, TxEventLog)]
  , fesFailures :: MVar [(TransactionHash, TxFailureDiagnostic)]
  , fesServer :: RpcServer
  }

fesPort :: FakeEventServer -> Word16
fesPort = rpcServerPort . fesServer

startFakeEventServer :: IO FakeEventServer
startFakeEventServer = do
  logs <- newMVar []
  failures <- newMVar []
  srv <- bindRpcServer "127.0.0.1" 0
  let impl =
        EventExecutor1
          { eventexecutor_null = pure ()
          , eventexecutor_open_block = \_ -> pure KRPCNone
          , eventexecutor_close_block = \_ -> pure KRPCNone
          , eventexecutor_report_tx_failure = \h d -> modifyMVar_ failures (pure . ((h, d) :)) >> pure KRPCNone
          , eventexecutor_emit_event = \bid tx logv _ -> modifyMVar_ logs (pure . ((bid, tx, logv) :)) >> pure KRPCNone
          }
  startAccepting srv defaultMaxMessageSize [eventExecutor1_service impl]
  pure (FakeEventServer logs failures srv)

stopFakeEventServer :: FakeEventServer -> IO ()
stopFakeEventServer = stopRpcServer . fesServer

-- | Every emitted event log, oldest first.
fesEventLogs :: FakeEventServer -> IO [(BlockID, TransactionID, TxEventLog)]
fesEventLogs s = reverse <$> readMVar (fesLogs s)

-- | The log emitted for one transaction, if any.
fesEventLog :: FakeEventServer -> BlockID -> TransactionID -> IO (Maybe TxEventLog)
fesEventLog s bid tx = do
  logs <- readMVar (fesLogs s)
  pure (case [l | (b, t, l) <- logs, b == bid, t == tx] of (l : _) -> Just l; [] -> Nothing)

-- | Every failure report, oldest first (sent asynchronously by the
-- executor, so a test may need to wait for one).
fesFailureReports :: FakeEventServer -> IO [(TransactionHash, TxFailureDiagnostic)]
fesFailureReports s = reverse <$> readMVar (fesFailures s)
