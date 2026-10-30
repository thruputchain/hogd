-- | Executor metrics (hog_exec/shared.h, runtime/metrics.h): process-wide
-- counters, rendered for the metrics HTTP endpoint.
module Hogd.Exec.Metrics
  ( Counter
  , successfulProposedTx
  , failedProposedTx
  , successfulValidationTx
  , failedValidationTx
  , gasConsumed
  , incrementCounter
  , readCounter
  , markTxSuccess
  , markTxFailed
  , allExecutorCounters
  ) where

import Data.IORef
import Data.Word
import Hogd.Exec.Common.Status (InvokeResult (..))
import Hogd.Exec.Log (logError)
import Hogd.Xdr.RpcTypes (BlockExecutionMode, pattern KBlockExecutionModeProposal, pattern KBlockExecutionModeValidation)
import System.IO.Unsafe (unsafePerformIO)

data Counter = Counter
  { counterName :: String
  , counterHelp :: String
  , counterRef :: IORef Word64
  }

mkCounter :: String -> String -> Counter
mkCounter n h = Counter n h (unsafePerformIO (newIORef 0))
{-# NOINLINE mkCounter #-}

successfulProposedTx, failedProposedTx, successfulValidationTx, failedValidationTx, gasConsumed :: Counter
successfulProposedTx = mkCounter "successful_proposed_tx_count" ""
{-# NOINLINE successfulProposedTx #-}
failedProposedTx = mkCounter "failed_proposed_tx_count" ""
{-# NOINLINE failedProposedTx #-}
successfulValidationTx =
  mkCounter
    "successful_validation_tx_count"
    "Transactions that passed validation-path execution. The proposed-tx counters move only for the proposer."
{-# NOINLINE successfulValidationTx #-}
failedValidationTx = mkCounter "failed_validation_tx_count" "Transactions that failed validation-path execution."
{-# NOINLINE failedValidationTx #-}
gasConsumed = mkCounter "gas_consumed" ""
{-# NOINLINE gasConsumed #-}

allExecutorCounters :: [(String, String, IO Word64)]
allExecutorCounters =
  [ (counterName c, counterHelp c, readCounter c)
  | c <- [successfulProposedTx, failedProposedTx, successfulValidationTx, failedValidationTx, gasConsumed]
  ]

incrementCounter :: Counter -> Word64 -> IO ()
incrementCounter c n = atomicModifyIORef' (counterRef c) (\v -> (v + n, ()))

readCounter :: Counter -> IO Word64
readCounter = readIORef . counterRef

markTxSuccess :: InvokeResult -> BlockExecutionMode -> IO ()
markTxSuccess r mode = do
  case mode of
    KBlockExecutionModeProposal -> incrementCounter successfulProposedTx 1
    KBlockExecutionModeValidation -> incrementCounter successfulValidationTx 1
    _ -> logError "mark_tx_success: received BlockExecutionMode::kBlockExecutionModeViewing"
  incrementCounter gasConsumed (irGasConsumed r)

markTxFailed :: InvokeResult -> BlockExecutionMode -> IO ()
markTxFailed r mode = do
  case mode of
    KBlockExecutionModeProposal -> incrementCounter failedProposedTx 1
    KBlockExecutionModeValidation -> incrementCounter failedValidationTx 1
    _ -> logError "mark_tx_failed: received BlockExecutionMode::kBlockExecutionModeViewing"
  incrementCounter gasConsumed (irGasConsumed r)
