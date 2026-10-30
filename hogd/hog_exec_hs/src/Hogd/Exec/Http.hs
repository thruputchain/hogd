-- | The metrics and health HTTP endpoint (hogd/http, hogd/metrics/shared.h):
-- GET /metrics serves Prometheus text, GET /health a JSON status document.
module Hogd.Exec.Http
  ( HealthStatus (..)
  , setHealthStatus
  , startMetricsServer
  , renderPrometheus
  , renderHealth
  ) where

import Control.Concurrent (forkFinally, forkIO)
import Control.Exception (SomeException, try)
import Control.Monad (forever, void)
import qualified Data.ByteString.Char8 as BC
import Data.IORef
import Data.List (sortOn)
import Data.Time.Clock (getCurrentTime)
import Data.Time.Format (defaultTimeLocale, formatTime)
import Data.Word
import Hogd.Exec.Metrics (allExecutorCounters)
import Network.Socket
import qualified Network.Socket.ByteString as NSB
import System.Environment (getExecutablePath, getProgName)
import System.IO.Unsafe (unsafePerformIO)
import System.Posix.Process (getProcessID)
import X23.Rpc (resolveHostPort)

data HealthStatus = Starting | Ok | HealthError

{-# NOINLINE healthRef #-}
healthRef :: IORef HealthStatus
healthRef = unsafePerformIO (newIORef Starting)

setHealthStatus :: HealthStatus -> IO ()
setHealthStatus = writeIORef healthRef

-- | Every executor metric, named "executor_<metric>".
renderPrometheus :: IO String
renderPrometheus = do
  rows <- mapM (\(n, h, rd) -> (,,) n h <$> rd) allExecutorCounters
  rss <- residentBytes
  let counters =
        concat
          [ (if null h then "" else "# HELP executor_" ++ n ++ " " ++ h ++ "\n")
              ++ "# TYPE executor_" ++ n ++ " counter\n"
              ++ "executor_" ++ n ++ " " ++ show v ++ "\n"
          | (n, h, v) <- sortOn (\(n, _, _) -> n) rows
          ]
      gauge =
        "# HELP executor_resident_bytes Resident set size of this process, in bytes.\n"
          ++ "# TYPE executor_resident_bytes gauge\n"
          ++ "executor_resident_bytes "
          ++ show rss
          ++ "\n"
  pure (counters ++ gauge)

residentBytes :: IO Integer
residentBytes = do
  r <- try (readFile "/proc/self/statm")
  pure $ case r of
    Left (_ :: SomeException) -> 0
    Right s -> case words s of
      (_ : resident : _) -> read resident * 4096
      _ -> 0

renderHealth :: IO String
renderHealth = do
  st <- readIORef healthRef
  name <- getProgName
  pid <- getProcessID
  exe <- getExecutablePath
  now <- getCurrentTime
  let status = case st of Starting -> "starting"; Ok -> "ok"; HealthError -> "error"
  pure $
    "{\"process_name\":" ++ show name ++ ",\"pid\":" ++ show pid ++ ",\"executable_path\":" ++ show exe
      ++ ",\"last_updated_timestamp\":"
      ++ show (formatTime defaultTimeLocale "%Y-%m-%dT%H:%M:%S%QZ" now)
      ++ ",\"status\":"
      ++ show (status :: String)
      ++ ",\"current_block_height\":0}"

-- | Serve the routes; returns the bound port.
startMetricsServer :: String -> Word16 -> IO Word16
startMetricsServer host port = do
  addr <- resolveHostPort host port
  sock <- socket (addrFamily addr) Stream defaultProtocol
  setSocketOption sock ReuseAddr 1
  bind sock (addrAddress addr)
  listen sock 128
  actual <- socketPort sock
  void $ forkIO $ forever $ do
    (conn, _) <- accept sock
    void $ forkFinally (serve conn) (\_ -> close conn)
  pure (fromIntegral actual)
  where
    serve conn = do
      req <- NSB.recv conn 8192
      let path = case words (BC.unpack (BC.takeWhile (/= '\r') req)) of
            (_ : p : _) -> p
            _ -> ""
      (status, ctype, body) <- case path of
        "/metrics" -> (\b -> ("200 OK", "text/plain", b)) <$> renderPrometheus
        "/health" -> (\b -> ("200 OK", "application/json", b)) <$> renderHealth
        _ -> pure ("404 Not Found", "text/plain", "not found\n")
      let bodyB = BC.pack body
      NSB.sendAll conn $
        BC.pack
          ( "HTTP/1.1 " ++ status ++ "\r\nContent-Type: " ++ ctype ++ "\r\nContent-Length: " ++ show (BC.length bodyB)
              ++ "\r\nConnection: close\r\n\r\n"
          )
          <> bodyB
