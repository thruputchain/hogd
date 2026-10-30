-- | One invocation frame's view of one storage item
-- (runtime/item/item_view.{h,cc}).
--
-- Views are immutable values here; the frame stack stores the updated view
-- back after each operation.  A frame copies its parent's view on first
-- touch, so reverting a frame drops its views.
module Hogd.Exec.Runtime.Item.ItemView
  ( ItemView (..)
  , ItemState (..)
  , RetrocausalQueueItem (..)
  , emptyItemView
  , setItem
  , hasPendingWrite
  , setFlags
  , clearFlags
  , itemDelta
  , syncRetrocausallyConsumed
  , unmodifiedItemType
    -- * Retrocausal queues
  , newRcqItem
  , makeFullyConsumedItem
  , rcqGetNextItem
  , rcqInsertEntry
  , rcqDelta
  ) where

import Data.Bits ((.|.))
import qualified Data.ByteString as B
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Exec.Common.Int128
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ExecutionMode
import Hogd.Exec.Log
import Hogd.Exec.Runtime.Item.SnapshotStorageItem
import Hogd.Xdr.AddressAuth (PublicKeyConfigOpt)
import Hogd.Xdr.AuthClaim (Weight)
import Hogd.Xdr.RetrocausalQueue
import Hogd.Xdr.StorageItem
import Hogd.Xdr.Transaction (Uniquifier)

-- | The pending state of an item, one constructor per C++ item class.
data ItemState
  = Unmodified !(Maybe SnapshotStorageItem)
  -- ^ not written this transaction; the snapshot item once loaded
  | DeletedItem
  | BytestringItem !B.ByteString
  | NNIntItem !(Maybe Integer) !Integer
  -- ^ set value (if any) and add value
  | NativeTokenBalanceItem !(Maybe Integer) !Integer
  | RcqItem !RetrocausalQueueItem
  | ReplayCacheItem !Uniquifier
  | AddressAuthDataItem !(M.Map Word32 PublicKeyConfigOpt) !(Maybe Weight)

data ItemView = ItemView
  { ivState :: !ItemState
  , ivFlags :: !StorageDeltaFlagSet
  -- ^ flags every delta from this view carries
  , ivRetroConsumed :: !Word32
  -- ^ retrocausally consumed entries before a delete-and-recreate, so they
  -- are never consumed twice
  }

emptyItemView :: ItemView
emptyItemView = ItemView (Unmodified Nothing) 0 0

-- | The type of a loaded unmodified item's snapshot base.
unmodifiedItemType :: SnapshotStorageItem -> Maybe ItemViewType
unmodifiedItemType = snapshotItemType

-- | Replace the pending write, validating types that carry a validate().
-- The view's flags are left alone.
setItem :: ItemState -> ItemView -> IO (SyscallStatus ItemView)
setItem st v = do
  ok <- validate st
  pure $ case ok of
    Left e -> Left e
    Right () -> Right v {ivState = st}

validate :: ItemState -> IO (SyscallStatus ())
validate = \case
  BytestringItem b -> do
    -- The ceiling, not the limit in force: items outlive a lowered limit.
    checkM (B.length b <= kBytestringCeilingLen) "bytestring exceeds ceiling"
    pure (Right ())
  NNIntItem base add -> pure (validateNNInt base add)
  NativeTokenBalanceItem base add -> pure (validateNNInt base add)
  _ -> pure (Right ())

-- | A nonnegative add is always valid, even over a negative set value (the
-- invariant is value >= min(set_value, 0), #2155).  A negative add needs a
-- nonnegative base that it does not drive below zero.
validateNNInt :: Maybe Integer -> Integer -> SyscallStatus ()
validateNNInt base add
  | add >= 0 = Right ()
  | otherwise = case base of
      Nothing -> Right ()
      Just b
        | b < 0 -> Left KSyscallErrorCodeInvalidSyscallInput
        | b + add < 0 -> Left KSyscallErrorCodeInvalidSyscallInput
        | otherwise -> Right ()

hasPendingWrite :: ItemView -> Bool
hasPendingWrite v = case ivState v of
  Unmodified _ -> False
  _ -> True

-- | OR in flags; fails on a view with no pending write.
setFlags :: StorageDeltaFlagSet -> ItemView -> SyscallStatus ItemView
setFlags f v
  | not (hasPendingWrite v) = Left KSyscallErrorCodeInvalidSyscallInput
  | otherwise = Right v {ivFlags = ivFlags v .|. f}

clearFlags :: ItemView -> ItemView
clearFlags v = v {ivFlags = 0}

syncRetrocausallyConsumed :: RetrocausalQueueItem -> ItemView -> ItemView
syncRetrocausallyConsumed q v = v {ivRetroConsumed = fromIntegral (rqLocallyConsumed q)}

-- | The delta this view would commit, with any uncovered observations, and
-- its flags.
itemDelta :: [QueueEntry] -> ItemView -> (Maybe StorageDelta, StorageDeltaFlagSet)
itemDelta observed v = (delta, ivFlags v)
  where
    delta = case ivState v of
      Unmodified _
        | null observed -> Nothing
        | otherwise ->
            Just
              ( StorageDelta_retrocausal_queue
                  RetrocausalQueueDelta
                    { consume = Nothing
                    , insert = RetrocausalQueueInsertDelta {added_entries = []}
                    , observed_concurrent_entries = observed
                    }
              )
      DeletedItem -> Just StorageDelta_kDeltaTypeDelete
      BytestringItem b -> Just (StorageDelta_bytestring b)
      NNIntItem base add -> Just (nnintDelta base add)
      NativeTokenBalanceItem base add -> Just (nnintDelta base add)
      RcqItem q -> Just (StorageDelta_retrocausal_queue (rcqDelta observed q))
      ReplayCacheItem u -> Just (StorageDelta_consume_uniquifier_args ConsumeUniquifierArgs {uniquifier = u})
      AddressAuthDataItem changes weight ->
        Just
          ( StorageDelta_auth_data_change
              AuthDataChangeArgs
                { auth_data_change_signers =
                    [AuthDataChangeSigner {public_key_id = k, public_key = pk} | (k, pk) <- M.toList changes]
                , new_required_weight = weight
                }
          )
    nnintDelta base add = case base of
      Just b -> StorageDelta_nnint NonnegativeInt128SetAddArgs {set_value = toXdr128 b, add_value = toXdr128 add}
      Nothing -> StorageDelta_nnint_add (toXdr128 add)

------------------------------------------------------------------------------
-- Retrocausal queues

data RetrocausalQueueItem = RetrocausalQueueItem
  { rqConsume :: !(Maybe RetrocausalQueueConsumeDelta)
  , rqInserted :: ![QueueEntry]
  -- ^ entries this transaction inserted, in order
  , rqSelfConsumed :: !Int
  -- ^ how many of 'rqInserted' this transaction consumed
  , rqLocallyConsumed :: !Int
  -- ^ retrocausally consumed concurrent entries from other transactions
  , rqPastBlockConsumed :: !Word32
  , rqBase :: !SnapshotStorageItem
  }

newRcqItem :: SnapshotStorageItem -> RetrocausalQueueItem
newRcqItem base = RetrocausalQueueItem Nothing [] 0 0 0 base

-- | A fresh queue over a fully consumed view of the snapshot's queue, for a
-- key this transaction replaced (make_fully_consumed_item).
makeFullyConsumedItem :: SnapshotStorageItem -> [QueueEntry] -> IO RetrocausalQueueItem
makeFullyConsumedItem base consumed = do
  checkM (length consumed <= kRetrocausalQueueBlockEntriesMaxLen) "consumed entry count"
  let count = length consumed
      meta = case ssiEntry base of
        SERetrocausalQueue m -> Just m
        _ -> Nothing
      needConsume = count > 0 || maybe False (\m -> m.live_entries > 0) meta
      past =
        RetrocausalQueuePastBlockConsumeDelta
          { max_fully_consumed_block_number = maybe 0 (\m -> m.max_live_block_num) meta
          , partial_consumed_number = 0
          }
  pure
    (newRcqItem base)
      { rqLocallyConsumed = count
      , rqConsume =
          if needConsume
            then Just RetrocausalQueueConsumeDelta {past_block_consume = past, current_block_consumed = consumed}
            else Nothing
      }

constructConsumeDelta :: RetrocausalQueueItem -> RetrocausalQueueItem
constructConsumeDelta q =
  let past = case ssiEntry (rqBase q) of
        SERetrocausalQueue m -> m.consume_state
        _ -> RetrocausalQueuePastBlockConsumeDelta {max_fully_consumed_block_number = 0, partial_consumed_number = 0}
   in q {rqConsume = Just RetrocausalQueueConsumeDelta {past_block_consume = past, current_block_consumed = []}}

-- | The next entry to consume, in order: (1) past blocks, (2) entries this
-- transaction inserted, (3) concurrent entries from other transactions.
-- Returns the updated item (cursor advanced if requested).  Nothing means
-- nothing to consume (or the per-transaction observation cap was reached).
rcqGetNextItem
  :: ExecutionModeData -> Bool -> RetrocausalQueueItem -> IO (SystemStatus (Maybe QueueEntry, RetrocausalQueueItem))
rcqGetNextItem mode advance q0 = do
  let base = rqBase q0
      (blockIndex, partial) = case rqConsume q0 of
        Just c -> (c.past_block_consume.max_fully_consumed_block_number + 1, c.past_block_consume.partial_consumed_number)
        Nothing -> case ssiEntry base of
          SERetrocausalQueue m -> (m.consume_state.max_fully_consumed_block_number + 1, m.consume_state.partial_consumed_number)
          _ -> (1, 0)
  nb <- getNextRcqBlock base blockIndex
  case nb of
    Left e -> pure (Left e)
    Right (Just blk) -> do
      let entries = blk.entries
          n = fromIntegral (length entries) :: Word64
      if partial >= n
        then do
          logError
            ( "Unrecoverable: recovered RCQ consume cursor " ++ show partial ++ " is past block "
                ++ show blk.block_number ++ " entry count " ++ show n
            )
          pure (Left SysUnrecoverable)
        else do
          let entry = entries !! fromIntegral partial
          if not advance
            then pure (Right (Just entry, q0))
            else do
              let q1 = maybe (constructConsumeDelta q0) (const q0) (rqConsume q0)
                  Just c = rqConsume q1
                  p = c.past_block_consume
                  partial' = p.partial_consumed_number + 1
                  p' =
                    if partial' == n
                      then p {max_fully_consumed_block_number = blk.block_number, partial_consumed_number = 0}
                      else p {partial_consumed_number = partial'}
              pure
                ( Right
                    ( Just entry
                    , q1
                        { rqConsume = Just c {past_block_consume = p'}
                        , rqPastBlockConsumed = rqPastBlockConsumed q1 + 1
                        }
                    )
                )
    Right Nothing ->
      -- Step 2: entries created within this transaction.
      if rqSelfConsumed q0 < length (rqInserted q0)
        then do
          let entry = rqInserted q0 !! rqSelfConsumed q0
          if not advance
            then pure (Right (Just entry, q0))
            else do
              checkM (rqSelfConsumed q0 < kRetrocausalQueueBlockEntriesMaxLen) "self consumed index"
              let q1 = q0 {rqSelfConsumed = rqSelfConsumed q0 + 1}
                  q2 = maybe (constructConsumeDelta q1) (const q1) (rqConsume q1)
              pure (Right (Just entry, q2))
        else do
          -- Step 3: concurrent entries from other transactions.
          ce <- nextConcurrentEntry mode (fromIntegral (rqLocallyConsumed q0)) base
          case ce of
            Left e -> pure (Left e)
            Right Nothing -> pure (Right (Nothing, q0))
            Right (Just entry)
              | not advance -> pure (Right (Just entry, q0))
              | otherwise -> do
                  let q1 = q0 {rqLocallyConsumed = rqLocallyConsumed q0 + 1}
                      q2 = maybe (constructConsumeDelta q1) (const q1) (rqConsume q1)
                      Just c = rqConsume q2
                  if length c.current_block_consumed >= kRetrocausalQueueBlockEntriesMaxLen
                    then do
                      checkM (not (isProposal mode)) "proposal exceeded block consumed cap"
                      pure (Left SysValidationFailure)
                    else
                      pure
                        ( Right
                            ( Just entry
                            , q2 {rqConsume = Just c {current_block_consumed = c.current_block_consumed ++ [entry]}}
                            )
                        )

-- | The next concurrent observation; the one place aware of the execution
-- mode for RCQ reads.
nextConcurrentEntry :: ExecutionModeData -> Word32 -> SnapshotStorageItem -> IO (SystemStatus (Maybe QueueEntry))
nextConcurrentEntry mode index base = do
  let addrkey = ssiAddrKey base
  capped <- observationCapExceeded mode
  if capped
    then pure (Right Nothing)
    else case mode of
      ModeProposal pd -> do
        out <- getConcurrentRcqEntry base index
        case out of
          Left e -> pure (Left e)
          Right Nothing -> do
            logNondeterministicBit pd False
            pure (Right Nothing)
          Right (Just live) -> do
            logNondeterministicBit pd True
            -- Only a recorded entry may be consumed (#3043).
            Right <$> logQueueEntryObservation pd addrkey index live
      ModeViewing vd -> do
        present <- viewQueryQueueObservationBit vd
        if not present
          then pure (Right Nothing)
          else do
            obs <- viewQueueEntryObservation vd addrkey index
            case obs of
              Just e -> pure (Right (Just e))
              Nothing -> replayExhausted mode
      ModeValidation vd -> do
        bit <- consumeQueueEntryPresenceBit vd addrkey index
        case bit of
          Nothing -> pure (Left SysValidationFailure)
          Just False -> pure (Right Nothing)
          Just True -> do
            obs <- consumeQueueEntryObservation vd addrkey index
            case obs of
              Just e -> pure (Right (Just e))
              Nothing -> replayExhausted mode
  where
    -- If the proposer stopped at exactly the cap, replay agrees with an empty
    -- read; otherwise the proposal is malformed (#2894).
    replayExhausted m = do
      c <- observationCapExceeded m
      pure (if c then Right Nothing else Left SysValidationFailure)

rcqInsertEntry :: QueueEntry -> RetrocausalQueueItem -> SyscallStatus RetrocausalQueueItem
rcqInsertEntry entry q
  | length (rqInserted q) >= kRetrocausalQueueBlockEntriesMaxLen = Left KSyscallErrorCodeInvalidSyscallInput
  | otherwise = Right q {rqInserted = rqInserted q ++ [entry]}

rcqDelta :: [QueueEntry] -> RetrocausalQueueItem -> RetrocausalQueueDelta
rcqDelta observed q =
  RetrocausalQueueDelta
    { consume = rqConsume q
    , insert = RetrocausalQueueInsertDelta {added_entries = drop (rqSelfConsumed q) (rqInserted q)}
    , observed_concurrent_entries = observed
    }
