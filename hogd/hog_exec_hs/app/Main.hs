-- | hog-exec-hs: the transaction executor (hogd/hog_exec/hog_exec.cc).
--
-- Registers with the coordinator, connects to the database shards and the
-- event server it is told about, and serves the ExecutorSched RPC; optionally
-- also serves the public view gateway.
module Main (main) where

import Control.Monad (forM_, unless, when)
import Data.Maybe (isJust, isNothing)
import Data.Word (Word16)
import Hogd.Exec.Http
import Hogd.Exec.Interface.Executor
import Hogd.Exec.Interface.ViewGateway
import Hogd.Exec.Log
import Hogd.Version (hogdVersion, protocolToken)
import Hogd.Xdr.ExecSched (executorSched1_service)
import Hogd.Xdr.ViewGateway (viewGateway1_service)
import System.Environment (getArgs, getProgName)
import System.Exit
import System.IO
import Text.Read (readMaybe)
import X23.Rpc

data Opts = Opts
  { optCoord :: String
  , optBindIp :: String
  , optMempoolPort :: Word16
  , optMetricsPort :: Word16
  , optDbExecutor :: Maybe String
  , optEventExecutor :: Maybe String
  , optViewGatewayPort :: Maybe Word16
  }

usageLine :: String -> String
usageLine prog =
  "usage: " ++ prog ++ " --coordinator-addr <host:port> --bind-ip <ip> [--mempool-port <port>] [--metrics-port <port>] "
    ++ "[--db-executor-addr <host:port> --event-executor-addr <host:port> --view-gateway-port <port>] [-h|--help] [--version]"

usage :: IO a
usage = do
  prog <- getProgName
  putStrLn (usageLine prog)
  exitFailure

parsePort :: String -> String -> IO Word16
parsePort label v = case readMaybe v :: Maybe Integer of
  Just p | p >= 0 && p <= 65535 && all (`elem` ['0' .. '9']) v -> pure (fromIntegral p)
  _ -> do
    hPutStrLn stderr ("Invalid " ++ label ++ ": " ++ v)
    usage

parseArgs :: [String] -> Opts -> IO Opts
parseArgs args o = case args of
  [] -> pure o
  ("--metrics-port" : v : rest) -> parsePort "--metrics-port" v >>= \p -> parseArgs rest o {optMetricsPort = p}
  ("--coordinator-addr" : v : rest) -> parseArgs rest o {optCoord = v}
  ("--bind-ip" : v : rest) -> parseArgs rest o {optBindIp = v}
  ("--mempool-port" : v : rest) -> parsePort "--mempool-port" v >>= \p -> parseArgs rest o {optMempoolPort = p}
  ("--db-executor-addr" : v : rest) -> parseArgs rest o {optDbExecutor = Just v}
  ("--event-executor-addr" : v : rest) -> parseArgs rest o {optEventExecutor = Just v}
  ("--view-gateway-port" : v : rest) -> parsePort "--view-gateway-port" v >>= \p -> parseArgs rest o {optViewGatewayPort = Just p}
  ("-p" : v : rest) -> parsePort "-p" v >>= \p -> parseArgs rest o {optMempoolPort = p}
  _ -> usage

-- | x23::HostPort::show: IPv6 hosts are bracketed.
showHostPort :: String -> Word16 -> String
showHostPort h p = (if ':' `elem` h then "[" ++ h ++ "]" else h) ++ ":" ++ show p

parseAddr :: String -> String -> IO (String, Word16)
parseAddr label s = maybe (panic ("Invalid " ++ label ++ ": " ++ s)) pure (parseHostPort s)

main :: IO ()
main = do
  args <- getArgs
  prog <- getProgName
  when (any (`elem` ["-h", "--help"]) args) $ putStrLn (usageLine prog) >> exitSuccess
  when (args == ["--version"]) $ putStrLn (prog ++ " " ++ hogdVersion) >> exitSuccess
  hSetBuffering stdout LineBuffering
  logInfo ("hogd version: " ++ hogdVersion ++ ", protocol: " ++ show protocolToken)
  logInfo "starting hog_exec!"
  o <- parseArgs args (Opts "" "" 0 0 Nothing Nothing Nothing)
  when (null (optCoord o) || null (optBindIp o)) usage

  metricsPort <- startMetricsServer (optBindIp o) (optMetricsPort o)
  logInfo ("PORT hog_exec.metrics=" ++ show metricsPort)

  (coordHost, coordPort) <- parseAddr "coordinator address" (optCoord o)
  let viewConfigured = isJust (optDbExecutor o) || isJust (optEventExecutor o) || isJust (optViewGatewayPort o)
  when (viewConfigured && (isNothing (optDbExecutor o) || isNothing (optEventExecutor o) || isNothing (optViewGatewayPort o))) $
    panic "--db-executor-addr, --event-executor-addr, and --view-gateway-port must be provided together"

  -- Bind first to learn the port, register and initialize, and only then
  -- accept: coordinator connections queue in the backlog meanwhile.
  rpc <- newExecutorRpc
  server <- bindRpcServer (optBindIp o) (optMempoolPort o)
  let port = rpcServerPort server
  unless (port == optMempoolPort o || optMempoolPort o == 0) $ panic "bound to an unexpected port"
  intro <- registerWithCoordinator coordHost coordPort (showHostPort (optBindIp o) port)
  initExecutorRpc rpc intro
  startAccepting server 0x7fffffff [executorSched1_service (executorRpcService rpc)]
  logInfo ("PORT hog_exec.mempool=" ++ show port)

  forM_ (if viewConfigured then [()] else []) $ \() -> do
    let Just dbAddr = optDbExecutor o
        Just evAddr = optEventExecutor o
        Just vgPort = optViewGatewayPort o
    (evHost, evPort) <- parseAddr "event executor address" evAddr
    vg <- newViewGatewayRpc dbAddr evHost evPort
    vgServer <- bindRpcServer (optBindIp o) vgPort
    startAccepting vgServer defaultMaxMessageSize [viewGateway1_service (viewGatewayService vg)]
    logInfo ("PORT hog_exec.view_gateway=" ++ show (rpcServerPort vgServer))

  setHealthStatus Ok
  logInfo "hog_exec initialization complete."
  awaitShutdownRequest rpc
  logInfo "hog_exec shutting down."
