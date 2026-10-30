-- | Logging, after hogd/base/log.h: a process-wide level and two output
-- formats (console, or JSON lines when HOGD_LOG_FORMAT=json), written to
-- stderr.
module Hogd.Exec.Log
  ( LogLevel (..)
  , setLogLevel
  , getLogLevel
  , logEnabled
  , logAt
  , logTrace
  , logDebug
  , logInfo
  , logWarn
  , logError
  , logCritical
  , panic
  , checkM
  ) where

import Control.Concurrent.MVar
import Control.Monad (when)
import Data.Char (ord)
import Data.IORef
import Data.Time.Clock (getCurrentTime)
import Data.Time.Clock.POSIX (getPOSIXTime)
import Data.Time.Format (defaultTimeLocale, formatTime)
import System.Environment (lookupEnv)
import System.Exit (ExitCode (..))
import System.Posix.Process (exitImmediately)
import System.IO
import System.IO.Unsafe (unsafePerformIO)
import Text.Printf (printf)

data LogLevel = Critical | Error | Warn | Info | Debug | Trace
  deriving (Eq, Ord, Show, Enum, Bounded)

levelChar :: LogLevel -> Char
levelChar = \case
  Critical -> 'C'
  Error -> 'E'
  Warn -> 'W'
  Info -> 'I'
  Debug -> 'D'
  Trace -> 'T'

levelName :: LogLevel -> String
levelName = \case
  Critical -> "critical"
  Error -> "error"
  Warn -> "warn"
  Info -> "info"
  Debug -> "debug"
  Trace -> "trace"

{-# NOINLINE levelRef #-}
levelRef :: IORef LogLevel
levelRef = unsafePerformIO (newIORef Info)

{-# NOINLINE outputLock #-}
outputLock :: MVar ()
outputLock = unsafePerformIO (newMVar ())

{-# NOINLINE jsonFormat #-}
jsonFormat :: Bool
jsonFormat = unsafePerformIO ((== Just "json") <$> lookupEnv "HOGD_LOG_FORMAT")

setLogLevel :: LogLevel -> IO ()
setLogLevel = writeIORef levelRef

getLogLevel :: IO LogLevel
getLogLevel = readIORef levelRef

logEnabled :: LogLevel -> IO Bool
logEnabled l = (l <=) <$> readIORef levelRef

logAt :: LogLevel -> String -> IO ()
logAt l msg = do
  on <- logEnabled l
  when on $ do
    line <-
      if jsonFormat
        then do
          ms <- (floor . (* 1000)) <$> getPOSIXTime :: IO Integer
          pure ("{\"timestamp_ms\":" ++ show ms ++ ",\"level\":\"" ++ levelName l ++ "\",\"message\":" ++ jsonString msg ++ "}")
        else do
          now <- getCurrentTime
          pure (levelChar l : formatTime defaultTimeLocale "[%m%d %H:%M:%S] " now ++ msg)
    withMVar outputLock $ \_ -> hPutStrLn stderr line >> hFlush stderr

jsonString :: String -> String
jsonString s = "\"" ++ concatMap esc s ++ "\""
  where
    esc '"' = "\\\""
    esc '\\' = "\\\\"
    esc '\n' = "\\n"
    esc '\r' = "\\r"
    esc '\t' = "\\t"
    esc c
      | ord c < 0x20 = printf "\\u%04x" (ord c)
      | otherwise = [c]

logTrace, logDebug, logInfo, logWarn, logError, logCritical :: String -> IO ()
logTrace = logAt Trace
logDebug = logAt Debug
logInfo = logAt Info
logWarn = logAt Warn
logError = logAt Error
logCritical = logAt Critical

-- | Log a critical message and terminate the process (hogd's panic()).
panic :: String -> IO a
panic msg = do
  logCritical ("panic: " ++ msg)
  hFlush stderr
  exitImmediately (ExitFailure 134)
  error "unreachable"

-- | CHECK(): a failed invariant terminates the process.
checkM :: Bool -> String -> IO ()
checkM ok msg = if ok then pure () else panic ("CHECK failed: " ++ msg)
