-- | Per-transaction bookkeeping of nondeterministic choices
-- (hogd/hog_exec/components/execution_mode.{h,cc}).
--
-- A proposer records each nondeterministic outcome (TryInvoke lock results,
-- concurrent retrocausal-queue reads); a validator replays them from the
-- proposal; a view consumes whatever partial data it was given and defaults
-- the rest.
module Hogd.Exec.Components.ExecutionMode
  ( ExecutionMode (..)
  , ExecutionModeCtor (..)
  , ExecutionModeData (..)
  , ProposalModeData
  , ValidationModeData
  , ViewingModeData
  , newExecutionModeData
  , isProposal
  , isValidation
  , isViewing
    -- * Proposal mode
  , logNondeterministicBit
  , logQueueEntryObservation
  , proposalData
    -- * Validation mode
  , validateProposalData
  , consumeNondeterministicBit
  , consumeQueueEntryPresenceBit
  , consumeQueueEntryObservation
  , getObservedQueueEntries
  , checkAllConsumed
    -- * Viewing mode
  , viewTryInvokeBit
  , viewQueryQueueObservationBit
  , viewQueueEntryObservation
  , viewGetObservedQueueEntries
    -- * Common
  , observationsToValidate
  , observationCapExceeded
  ) where

import Control.Monad (when)
import Data.Bits
import qualified Data.ByteString as B
import Data.Foldable (toList)
import Data.IORef
import qualified Data.Map.Strict as M
import Data.Sequence (Seq, (|>))
import qualified Data.Sequence as Seq
import Data.Word
import Hogd.Exec.Log (checkM)
import Hogd.Xdr

data ExecutionMode = Proposing | Validating | Viewing
  deriving (Eq, Show)

data ExecutionModeCtor
  = ProposalModeCtor
  | ValidationModeCtor NondeterministicProposalData
  | ViewingModeCtor NondeterministicProposalData

data ExecutionModeData
  = ModeProposal ProposalModeData
  | ModeValidation ValidationModeData
  | ModeViewing ViewingModeData

isProposal, isValidation, isViewing :: ExecutionModeData -> Bool
isProposal = \case ModeProposal _ -> True; _ -> False
isValidation = \case ModeValidation _ -> True; _ -> False
isViewing = \case ModeViewing _ -> True; _ -> False

newExecutionModeData :: ExecutionModeCtor -> IO ExecutionModeData
newExecutionModeData = \case
  ProposalModeCtor -> ModeProposal <$> newProposal
  ValidationModeCtor d -> ModeValidation <$> newValidation d
  ViewingModeCtor d -> ModeViewing . ViewingModeData <$> newValidation d

offsetBit :: Word32 -> Word8
offsetBit idx = 1 `shiftL` fromIntegral (idx `mod` 8)

------------------------------------------------------------------------------
-- Proposal

data ProposalState = ProposalState
  { psBits :: !(Seq Word8)
  , psBitCount :: !Word32
  , psSequence :: !(Seq QueueEntry)
  , psObservations :: !(M.Map AddressAndKey (Seq QueueEntry))
  , psCapExceeded :: !Bool
  }

newtype ProposalModeData = ProposalModeData (IORef ProposalState)

newProposal :: IO ProposalModeData
newProposal = ProposalModeData <$> newIORef (ProposalState Seq.empty 0 Seq.empty M.empty False)

-- | Records a nondeterministic boolean choice (e.g. does a try_invoke frame
-- succeed?).
logNondeterministicBit :: ProposalModeData -> Bool -> IO ()
logNondeterministicBit (ProposalModeData ref) bit = modifyIORef' ref $ \s ->
  let byteCount = fromIntegral (psBitCount s `div` 8) + 1
      bits0 = if byteCount /= Seq.length (psBits s) then psBits s |> 0 else psBits s
      bits1 =
        if bit
          then Seq.adjust' (.|. offsetBit (psBitCount s)) (Seq.length bits0 - 1) bits0
          else bits0
   in s {psBits = bits1, psBitCount = psBitCount s + 1}

-- | Records a concurrent queue-entry observation and returns the recorded
-- entry, or Nothing (recording nothing) once the per-transaction cap would be
-- exceeded (#2894).  Re-observing a recorded (queue, index) returns the
-- recorded value without appending it again.
logQueueEntryObservation :: ProposalModeData -> AddressAndKey -> Word32 -> QueueEntry -> IO (Maybe QueueEntry)
logQueueEntryObservation (ProposalModeData ref) addrkey index entry = do
  s <- readIORef ref
  let existing = M.lookup addrkey (psObservations s)
  case existing of
    Just es | fromIntegral index < Seq.length es -> pure (Just (Seq.index es (fromIntegral index)))
    _
      | Seq.length (psSequence s) >= kMaxConcurrentRcqReadsPerTx -> do
          writeIORef ref s {psCapExceeded = True}
          pure Nothing
      | otherwise -> do
          let es = maybe Seq.empty id existing
          checkM (Seq.length es == fromIntegral index) "queue observation index out of order"
          writeIORef
            ref
            s
              { psObservations = M.insert addrkey (es |> entry) (psObservations s)
              , psSequence = psSequence s |> entry
              }
          pure (Just entry)

proposalData :: ProposalModeData -> IO NondeterministicProposalData
proposalData (ProposalModeData ref) = do
  s <- readIORef ref
  pure
    NondeterministicProposalData
      { nondeterministic_bits = B.pack (toList (psBits s))
      , nondeterministic_bit_count = psBitCount s
      , observed_queue_entry_sequence = toList (psSequence s)
      }

------------------------------------------------------------------------------
-- Validation

data ValidationState = ValidationState
  { vsNextBit :: !Word32
  , vsNextEntry :: !Int
  , vsObservations :: !(M.Map AddressAndKey (Seq QueueEntry))
  , vsCapExceeded :: !Bool
  }

data ValidationModeData = ValidationModeData
  { vmData :: !NondeterministicProposalData
  , vmSequence :: !(Seq QueueEntry)
  , vmState :: !(IORef ValidationState)
  }

newValidation :: NondeterministicProposalData -> IO ValidationModeData
newValidation d =
  ValidationModeData d (Seq.fromList d.observed_queue_entry_sequence)
    <$> newIORef (ValidationState 0 0 M.empty False)

expectedByteCount :: Word32 -> Int
expectedByteCount n = fromIntegral (n `div` 8) + (if n `mod` 8 == 0 then 0 else 1)

-- | Well-formedness of the supplied proposal data: the byte count matches the
-- bit count and unused trailing bits are zero (no entropy grinding).
validateProposalData :: ValidationModeData -> Bool
validateProposalData v =
  let d = vmData v
      bits = d.nondeterministic_bits
      n = d.nondeterministic_bit_count
   in B.length bits == expectedByteCount n
        && ( B.null bits
              || let trailing = n `mod` 8
                     mask = (0xff `shiftL` fromIntegral trailing) :: Word8
                  in trailing == 0 || (B.last bits .&. mask) == 0
           )

-- | Consumes the next nondeterministic bit; Nothing when exhausted (a
-- validation failure).
consumeNondeterministicBit :: ValidationModeData -> IO (Maybe Bool)
consumeNondeterministicBit v = do
  s <- readIORef (vmState v)
  let d = vmData v
      idx = vsNextBit s
      byteIdx = fromIntegral (idx `div` 8)
  if idx >= d.nondeterministic_bit_count || B.length d.nondeterministic_bits <= byteIdx
    then pure Nothing
    else do
      writeIORef (vmState v) s {vsNextBit = idx + 1}
      pure (Just ((B.index d.nondeterministic_bits byteIdx .&. offsetBit idx) /= 0))

-- | The presence bit for a concurrent read at (addrkey, index): Nothing if
-- bits are exhausted or an absent bit contradicts a held entry.
consumeQueueEntryPresenceBit :: ValidationModeData -> AddressAndKey -> Word32 -> IO (Maybe Bool)
consumeQueueEntryPresenceBit v addrkey index = do
  bit <- consumeNondeterministicBit v
  case bit of
    Just False -> do
      s <- readIORef (vmState v)
      case M.lookup addrkey (vsObservations s) of
        Just es | fromIntegral index < Seq.length es -> pure Nothing
        _ -> pure bit
    _ -> pure bit

-- | The next observed queue entry for (addrkey, index), or Nothing if the
-- recorded sequence is exhausted (flagging the cap if it is full).
consumeQueueEntryObservation :: ValidationModeData -> AddressAndKey -> Word32 -> IO (Maybe QueueEntry)
consumeQueueEntryObservation v addrkey index = do
  s <- readIORef (vmState v)
  let es = M.findWithDefault Seq.empty addrkey (vsObservations s)
      s0 = s {vsObservations = M.insert addrkey es (vsObservations s)}
  if fromIntegral index < Seq.length es
    then do
      writeIORef (vmState v) s0
      pure (Just (Seq.index es (fromIntegral index)))
    else
      if vsNextEntry s >= Seq.length (vmSequence v)
        then do
          let capped = Seq.length (vmSequence v) >= kMaxConcurrentRcqReadsPerTx
          writeIORef (vmState v) s0 {vsCapExceeded = vsCapExceeded s0 || capped}
          pure Nothing
        else do
          let e = Seq.index (vmSequence v) (vsNextEntry s)
          writeIORef
            (vmState v)
            s0
              { vsObservations = M.insert addrkey (es |> e) (vsObservations s0)
              , vsNextEntry = vsNextEntry s + 1
              }
          pure (Just e)

getObservedQueueEntries :: ValidationModeData -> AddressAndKey -> Word32 -> IO [QueueEntry]
getObservedQueueEntries _ _ 0 = pure []
getObservedQueueEntries v addrkey count = do
  s <- readIORef (vmState v)
  case M.lookup addrkey (vsObservations s) of
    Nothing -> do
      checkM False "get_observed_queue_entries: no observations for key"
      pure []
    Just es -> do
      checkM (fromIntegral count <= Seq.length es) "get_observed_queue_entries: count"
      pure (toList (Seq.take (fromIntegral count) es))

-- | Whether the entirety of the nondeterministic data has been consumed.
checkAllConsumed :: ValidationModeData -> IO Bool
checkAllConsumed v = do
  s <- readIORef (vmState v)
  let d = vmData v
  pure $
    d.nondeterministic_bit_count == vsNextBit s
      && B.length d.nondeterministic_bits == expectedByteCount d.nondeterministic_bit_count
      && vsNextEntry s == Seq.length (vmSequence v)

------------------------------------------------------------------------------
-- Viewing

newtype ViewingModeData = ViewingModeData ValidationModeData

-- | WAI: view mode defaults missing TryInvoke bits to true (success).
viewTryInvokeBit :: ViewingModeData -> IO Bool
viewTryInvokeBit (ViewingModeData v) = maybe True id <$> consumeNondeterministicBit v

-- | Missing queue-observation bits default to an empty queue.
viewQueryQueueObservationBit :: ViewingModeData -> IO Bool
viewQueryQueueObservationBit (ViewingModeData v) = maybe False id <$> consumeNondeterministicBit v

viewQueueEntryObservation :: ViewingModeData -> AddressAndKey -> Word32 -> IO (Maybe QueueEntry)
viewQueueEntryObservation (ViewingModeData v) = consumeQueueEntryObservation v

viewGetObservedQueueEntries :: ViewingModeData -> AddressAndKey -> Word32 -> IO [QueueEntry]
viewGetObservedQueueEntries (ViewingModeData v) addrkey count = do
  es <- getObservedQueueEntries v addrkey count
  checkM (length es == fromIntegral count) "view observed entries count"
  pure es

------------------------------------------------------------------------------
-- Common

-- | Observations a validator must submit for block-close matching.  Proposal
-- mode has none.
observationsToValidate :: ExecutionModeData -> IO (M.Map AddressAndKey [QueueEntry])
observationsToValidate = \case
  ModeProposal _ -> pure M.empty
  ModeValidation v -> obs v
  ModeViewing (ViewingModeData v) -> obs v
  where
    obs v = fmap toList . vsObservations <$> readIORef (vmState v)

observationCapExceeded :: ExecutionModeData -> IO Bool
observationCapExceeded = \case
  ModeProposal (ProposalModeData ref) -> psCapExceeded <$> readIORef ref
  ModeValidation v -> vsCapExceeded <$> readIORef (vmState v)
  ModeViewing (ViewingModeData v) -> vsCapExceeded <$> readIORef (vmState v)

-- Silence an unused-binding warning for 'when' on some GHC versions.
_unused :: IO ()
_unused = when False (pure ())
