-- | Invocation frames and the frame stack (runtime/invocation_frame.{h,cc}).
--
-- Each frame either commits into its parent or reverts as a unit.  The stack
-- lives in the handler's 'schFrames' (active frame first).
module Hogd.Exec.Runtime.InvocationFrame
  ( newFrame
  , frameIsTryInvoke
  , frameIsReadOnly
  , frameShouldRevert
  , depthCapReached
  , frameLogEvent
  , frameDeployContract
  , frameCommittedContracts
  , frameCheckOwnsAuthToken
  , frameLookupAuthToken
  , frameAddAuthToken
  , frameTakeAuthToken
  , frameNextAuthTokenId
    -- * Stack operations
  , activeFrame
  , modifyActiveFrame
  , stackSize
  , stackEmpty
  , startFrameStack
  , finishFrameStack
  , getMsgSenderStack
  , hasActiveFrameFor
  , lookupLocallyDeployedContract
  , getItemView
  , putItemView
  , contractDataKey
  , mintAuthTokenStack
  , ConsumeAuthTokenResult (..)
  , consumeAuthTokenStack
  , checkAuthTokensOwned
  , checkParentCanTakeAuthTokens
  , setActiveFrameReturn
  , tryAcquireFrameLocks
  ) where

import Control.Monad (forM_, when)
import Data.Bits ((.&.))
import Data.Foldable (toList)
import Data.IORef
import qualified Data.Map.Strict as M
import Data.Sequence (Seq, (|>))
import qualified Data.Sequence as Seq
import qualified Data.Set as S
import Data.Word
import Hogd.Base.NetworkConfigParams (NetworkConfigParams (..))
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ExecutionMode
import Hogd.Exec.Log
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.ItemView
import Hogd.Exec.Runtime.Types
import Hogd.Xdr

newFrame
  :: ContractAddress -> Calldata -> InvokeFlagSet -> Int -> [AuthToken] -> Word32 -> NetworkConfigParams -> InvocationFrame
newFrame addr calldata flags eventCount forwarded parentDepth params =
  InvocationFrame
    { ifAddress = addr
    , ifCalldata = calldata
    , ifChildReturnOutput = Nothing
    , ifChildReturnedTokens = (0, 0)
    , ifToReturn = InvocationFrameOutput_return_buffer mempty
    , ifReturnedTokenIds = []
    , ifFlags = flags
    , ifEvents = Seq.empty
    , ifEventCount = eventCount
    , ifTryInvokeFrameCount = 0
    , ifTotalTryInvokeDepth = parentDepth
    , ifModifiedItems = M.empty
    , ifNewDeployments = M.empty
    , ifAuthTokens = Seq.fromList (map Just forwarded)
    , ifForwardedCount = fromIntegral (length forwarded)
    , ifChildForwarded = []
    , ifHashBuffers = newHashBuffers params.max_sha256_buffers
    }

flagSet :: InvokeFlag -> InvokeFlagSet -> Bool
flagSet (InvokeFlag f) set = set .&. fromIntegral f /= 0

frameIsTryInvoke, frameIsReadOnly :: InvocationFrame -> Bool
frameIsTryInvoke = flagSet TryInvoke . ifFlags
frameIsReadOnly = flagSet ReadOnly . ifFlags

frameShouldRevert :: InvocationFrame -> Bool
frameShouldRevert f = case ifToReturn f of
  InvocationFrameOutput_return_buffer _ -> False
  _ -> True

-- | One savepoint slot is reserved for the root commit's push; a depth of 0
-- leaves none for a TryInvoke.
depthCapReached :: NetworkConfigParams -> InvocationFrame -> Bool
depthCapReached params f =
  params.max_savepoint_depth == 0 || ifTotalTryInvokeDepth f >= params.max_savepoint_depth - 1

frameLogEvent :: TxEmittedEvent -> InvocationFrame -> InvocationFrame
frameLogEvent e f = f {ifEventCount = ifEventCount f + 1, ifEvents = ifEvents f |> e}

frameDeployContract :: ContractAddress -> DeployedContractHash -> InvocationFrame -> IO InvocationFrame
frameDeployContract addr h f = do
  checkM (not (M.member addr (ifNewDeployments f))) "deploy_contract: address already deployed"
  pure f {ifNewDeployments = M.insert addr h (ifNewDeployments f)}

frameCommittedContracts :: InvocationFrame -> S.Set SHA256Hash
frameCommittedContracts f = S.fromList [d.hash | d <- M.elems (ifNewDeployments f)]

frameCheckOwnsAuthToken :: Word32 -> InvocationFrame -> Bool
frameCheckOwnsAuthToken tid f = case Seq.lookup (fromIntegral tid) (ifAuthTokens f) of
  Just (Just _) -> True
  _ -> False

frameLookupAuthToken :: Word32 -> InvocationFrame -> Maybe AuthToken
frameLookupAuthToken tid f = case Seq.lookup (fromIntegral tid) (ifAuthTokens f) of
  Just t -> t
  Nothing -> Nothing

frameNextAuthTokenId :: InvocationFrame -> Int
frameNextAuthTokenId = Seq.length . ifAuthTokens

-- | Adds a token under the next id, or Nothing once ids are exhausted.
frameAddAuthToken :: AuthToken -> InvocationFrame -> Maybe (Word32, InvocationFrame)
frameAddAuthToken t f
  | Seq.length (ifAuthTokens f) >= fromIntegral (maxBound :: Word32) = Nothing
  | otherwise =
      Just (fromIntegral (Seq.length (ifAuthTokens f)), f {ifAuthTokens = ifAuthTokens f |> Just t})

frameTakeAuthToken :: Word32 -> InvocationFrame -> (Maybe AuthToken, InvocationFrame)
frameTakeAuthToken tid f =
  ( frameLookupAuthToken tid f
  , f {ifAuthTokens = Seq.update (fromIntegral tid) Nothing (ifAuthTokens f)}
  )

------------------------------------------------------------------------------
-- Merging frames

commitManuallyRevertedFrame :: InvocationFrame -> InvocationFrame -> InvocationFrame
commitManuallyRevertedFrame child parent =
  parent
    { ifChildReturnOutput = Just (ifToReturn child)
    , ifChildReturnedTokens = (0, 0)
    , ifChildForwarded = []
    }

commitFailedChildFrame :: InvocationFrame -> InvocationFrame
commitFailedChildFrame parent =
  parent
    { ifChildReturnOutput = Just InvocationFrameOutput_Fault
    , ifChildReturnedTokens = (0, 0)
    , ifChildForwarded = []
    }

commitSuccessfulChildFrame :: InvocationFrame -> InvocationFrame -> IO InvocationFrame
commitSuccessfulChildFrame child parent0 = do
  let parent1 =
        parent0
          { ifChildReturnOutput = Just (ifToReturn child)
          , ifEventCount = ifEventCount child
          , ifEvents = ifEvents parent0 <> ifEvents child
          , ifNewDeployments = M.union (ifNewDeployments parent0) (ifNewDeployments child)
          , ifModifiedItems = M.union (ifModifiedItems child) (ifModifiedItems parent0)
          , ifTryInvokeFrameCount = ifTryInvokeFrameCount parent0 + ifTryInvokeFrameCount child
          , ifTotalTryInvokeDepth = ifTotalTryInvokeDepth child
          }
      -- The tokens forwarded to the child leave this frame now.
      parent2 =
        foldl (\p tid -> snd (frameTakeAuthToken tid p)) parent1 (ifChildForwarded parent1)
      start = fromIntegral (frameNextAuthTokenId parent2)
      parent3 = parent2 {ifChildForwarded = [], ifChildReturnedTokens = (start, 0)}
  -- The tokens the child returned join this frame under consecutive new ids.
  let go p [] = pure p
      go p (ctid : rest) = case frameLookupAuthToken ctid child of
        Nothing -> panic "Return checked the child owns every returned token"
        Just t -> case frameAddAuthToken t p of
          Nothing -> panic "Return checked the parent had ids for every returned token"
          Just (_, p') ->
            let (s, n) = ifChildReturnedTokens p'
             in go p' {ifChildReturnedTokens = (s, n + 1)} rest
  go parent3 (ifReturnedTokenIds child)

------------------------------------------------------------------------------
-- Stack access

activeFrame :: SystemCallHandler -> IO InvocationFrame
activeFrame h =
  readIORef (schFrames h) >>= \case
    (f : _) -> pure f
    [] -> panic "no active invocation frame"

modifyActiveFrame :: SystemCallHandler -> (InvocationFrame -> InvocationFrame) -> IO ()
modifyActiveFrame h g = modifyIORef' (schFrames h) $ \case
  (f : rest) -> g f : rest
  [] -> []

stackSize :: SystemCallHandler -> IO Int
stackSize h = length <$> readIORef (schFrames h)

stackEmpty :: SystemCallHandler -> IO Bool
stackEmpty h = null <$> readIORef (schFrames h)

setActiveFrameReturn :: SystemCallHandler -> InvocationFrameOutput -> [Word32] -> IO ()
setActiveFrameReturn h out tokens = modifyActiveFrame h $ \f -> f {ifToReturn = out, ifReturnedTokenIds = tokens}

-- | Push a new frame.  The forwarded tokens must be owned by the parent; it
-- keeps them until the child commits.
startFrameStack :: SystemCallHandler -> ContractAddress -> Calldata -> InvokeFlagSet -> [Word32] -> IO ()
startFrameStack h addr calldata flags forwardedIds = do
  frames <- readIORef (schFrames h)
  let (eventCount, parentDepth) = case frames of
        (p : _) -> (ifEventCount p, ifTotalTryInvokeDepth p)
        [] -> (0, 0)
  forwarded <- case frames of
    (p : _) -> mapM (\tid -> maybe (panic "forwarded token not owned") pure (frameLookupAuthToken tid p)) forwardedIds
    [] -> do
      checkM (null forwardedIds) "root frame forwarded tokens"
      pure []
  let child = newFrame addr calldata flags eventCount forwarded parentDepth (schConfig h)
  writeIORef (schFrames h) $ case frames of
    (p : rest) -> child : p {ifChildForwarded = forwardedIds} : rest
    [] -> [child]

-- | InvocationFrameStack::finish_frame: settle TryInvoke locks, merge or
-- revert the child, pop it.  Returns the frame result and whether the frame
-- was manually reverted, plus a TryInvoke conflict diagnostic (proposal).
finishFrameStack
  :: SystemCallHandler -> InvokeStatus () -> (InvocationFrame -> IO ()) -> IO (SystemStatus (InvokeResult, Bool), DbConflictResult)
finishFrameStack h result lastFrameAction = do
  conflictRef <- newIORef DbConflictResult_kDbDiagnosticNone
  r <- go conflictRef
  c <- readIORef conflictRef
  pure (r, c)
  where
    mode = schMode h
    params = schConfig h
    db = schDb h
    popFrame = modifyIORef' (schFrames h) (drop 1)
    go conflictRef = do
      top <- activeFrame h
      let manuallyReverted = frameShouldRevert top
          initial = InvokeResult result 0
      -- TryInvoke: only when the deterministic part succeeded and the frame
      -- is not manually reverting.
      settled <-
        if frameIsTryInvoke top && isFrameSuccess initial && not manuallyReverted
          then do
            acq <-
              if depthCapReached params top
                then pure (Right False)
                else case mode of
                  ModeProposal pd -> do
                    r <- tryAcquireFrameLocks h M.empty
                    case r of
                      Left e -> pure (Left e)
                      Right ok -> do
                        ptxLastPushConflict db >>= writeIORef conflictRef
                        logNondeterministicBit pd ok
                        pure (Right ok)
                  ModeViewing vd -> Right <$> viewTryInvokeBit vd
                  ModeValidation vd -> do
                    bit <- consumeNondeterministicBit vd
                    case bit of
                      Nothing -> pure (Left SysValidationFailure)
                      Just False -> pure (Right False)
                      Just True -> do
                        obs <- observationsToValidate mode
                        top' <- activeFrame h
                        let bad =
                              or
                                [ case M.lookup ak (ifModifiedItems top') of
                                    Just v -> case ivState v of
                                      RcqItem _ -> False
                                      Unmodified _ -> False
                                      _ -> True
                                    Nothing -> False
                                | (ak, es) <- M.toList obs
                                , not (null es)
                                ]
                        pure (if bad then Left SysValidationFailure else Right True)
            case acq of
              Left e -> do
                popFrame
                pure (Left e)
              Right True -> do
                modifyActiveFrame h (\f -> f {ifTotalTryInvokeDepth = ifTotalTryInvokeDepth f + 1})
                pure (Right (initial, False))
              Right False -> pure (Right (InvokeResult (Left Deterministic) 0, True))
          else pure (Right (initial, False))
      case settled of
        Left e -> pure (Left e)
        Right (frameResult, tryInvokeFailed) -> do
          let revert = shouldFrameRevert frameResult
          failedOk <-
            if revert || manuallyReverted
              then do
                f <- activeFrame h
                markFrameAsFailed h f
              else pure (Right ())
          case failedOk of
            Left e -> do
              popFrame
              pure (Left e)
            Right ()
              | revert -> do
                  -- C++ asserts (debug only) that a reverting frame either
                  -- failed its TryInvoke or already reports Fault.
                  let _ = tryInvokeFailed
                  popFrame
                  modifyActiveFrame h commitFailedChildFrame
                  pure (Right (frameResult, manuallyReverted))
              | otherwise -> do
                  frames <- readIORef (schFrames h)
                  case frames of
                    (child : parent : rest) -> do
                      parent' <-
                        if frameShouldRevert child
                          then pure (commitManuallyRevertedFrame child parent)
                          else commitSuccessfulChildFrame child parent
                      writeIORef (schFrames h) (parent' : rest)
                    [child] -> do
                      lastFrameAction child
                      writeIORef (schFrames h) []
                    [] -> panic "finish_frame on empty stack"
                  pure (Right (frameResult, manuallyReverted))

-- | Pop the subtransactions this frame pushed for its TryInvoke children.
markFrameAsFailed :: SystemCallHandler -> InvocationFrame -> IO (SystemStatus ())
markFrameAsFailed h f
  | ifTryInvokeFrameCount f > 0 = ptxPopSubtransactions (schDb h) (ifTryInvokeFrameCount f)
  | otherwise = pure (Right ())

-- | Push the active frame's accumulated deltas (plus observation deltas) as
-- one subtransaction.  True on a successful lock acquisition.
tryAcquireFrameLocks :: SystemCallHandler -> M.Map AddressAndKey [QueueEntry] -> IO (SystemStatus Bool)
tryAcquireFrameLocks h observations = do
  f <- activeFrame h
  let db = schDb h
      items = M.toList (ifModifiedItems f)
      step [] = pure (Right ())
      step ((ak, view) : rest) = do
        let obs = M.lookup ak observations
            hasObs = maybe False (not . null) obs
            consumed = fromIntegral (ivRetroConsumed view)
        case obs of
          Just es -> checkM (consumed <= length es) "retrocausally consumed exceeds observations"
          Nothing -> pure ()
        let uncovered = maybe [] (drop consumed) obs
            (delta, flags) = itemDelta uncovered view
        case delta of
          Just d
            | hasObs && not (isRcqDelta d) -> pure (Left False)
            | otherwise -> ptxAddDelta db ak d flags >> step rest
          Nothing -> step rest
  r <- step items
  case r of
    Left _ -> pure (Right False)
    Right () -> do
      forM_ (M.toList observations) $ \(ak, es) ->
        when (not (null es) && not (M.member ak (ifModifiedItems f))) $
          ptxAddDelta
            db
            ak
            ( StorageDelta_retrocausal_queue
                RetrocausalQueueDelta
                  { consume = Nothing
                  , insert = RetrocausalQueueInsertDelta {added_entries = []}
                  , observed_concurrent_entries = es
                  }
            )
            0
      forM_ (M.toList (ifNewDeployments f)) $ \(addr, dep) ->
        ptxAddDelta db AddressAndKey {address = addr, key = StorageKey_Metadata} (StorageDelta_deployed_contract_hash dep) 0
      res <- ptxTryPushCurrentSubtransaction db
      case res of
        Left e -> pure (Left e)
        Right ok -> do
          when ok $ modifyActiveFrame h (\fr -> fr {ifTryInvokeFrameCount = ifTryInvokeFrameCount fr + 1})
          pure (Right ok)
  where
    isRcqDelta = \case StorageDelta_retrocausal_queue _ -> True; _ -> False

getMsgSenderStack :: SystemCallHandler -> IO (Maybe ContractAddress)
getMsgSenderStack h =
  readIORef (schFrames h) >>= \case
    (_ : p : _) -> pure (Just (ifAddress p))
    _ -> pure Nothing

hasActiveFrameFor :: SystemCallHandler -> ContractAddress -> IO Bool
hasActiveFrameFor h addr = any ((== addr) . ifAddress) <$> readIORef (schFrames h)

lookupLocallyDeployedContract :: SystemCallHandler -> ContractAddress -> IO (Maybe DeployedContractHash)
lookupLocallyDeployedContract h addr = do
  -- Search from the active frame outward.
  frames <- readIORef (schFrames h)
  pure $ case [d | f <- frames, Just d <- [M.lookup addr (ifNewDeployments f)]] of
    (d : _) -> Just d
    [] -> Nothing

contractDataKey :: ContractAddress -> StorageKeyContractData -> AddressAndKey
contractDataKey addr k = AddressAndKey {address = addr, key = StorageKey_contract_data_key k}

-- | The active frame's view of a key, created by copying the nearest
-- parent's view or empty.  The view is always present in the active frame
-- afterwards.
getItemView :: SystemCallHandler -> AddressAndKey -> IO ItemView
getItemView h ak = do
  frames <- readIORef (schFrames h)
  case frames of
    [] -> panic "get_item_view with no frame"
    (active : parents) -> case M.lookup ak (ifModifiedItems active) of
      Just v -> pure v
      Nothing -> do
        let v = case [pv | p <- parents, Just pv <- [M.lookup ak (ifModifiedItems p)]] of
              (pv : _) -> pv
              [] -> emptyItemView
        writeIORef (schFrames h) (active {ifModifiedItems = M.insert ak v (ifModifiedItems active)} : parents)
        pure v

putItemView :: SystemCallHandler -> AddressAndKey -> ItemView -> IO ()
putItemView h ak v = modifyActiveFrame h $ \f -> f {ifModifiedItems = M.insert ak v (ifModifiedItems f)}

mintAuthTokenStack :: SystemCallHandler -> AuthToken -> IO (SyscallStatus Word32)
mintAuthTokenStack h t =
  readIORef (schFrames h) >>= \case
    [] -> pure (Left KSyscallErrorCodeInvalidSyscallInput)
    (f : rest) -> case frameAddAuthToken t f of
      Nothing -> pure (Left KSyscallErrorCodeInvalidSyscallInput)
      Just (tid, f') -> do
        writeIORef (schFrames h) (f' : rest)
        pure (Right tid)

data ConsumeAuthTokenResult = ConsumeAuthTokenResult
  { catToken :: AuthToken
  , catNewIdStart :: Word32
  , catNewIdCount :: Word32
  }

consumeAuthTokenStack :: SystemCallHandler -> Word32 -> IO (SyscallStatus ConsumeAuthTokenResult)
consumeAuthTokenStack h tid = do
  f <- activeFrame h
  case frameLookupAuthToken tid f of
    Nothing -> pure (Left KSyscallErrorCodeInvalidSyscallInput)
    Just tok
      | tok.root_node.consumer_address /= ifAddress f -> pure (Left KSyscallErrorCodeInvalidSyscallInput)
      | otherwise -> do
          let children = tok.root_node.child_nodes
              childCount = length children
          if childCount > fromIntegral (maxBound :: Word32) - frameNextAuthTokenId f
            then pure (Left KSyscallErrorCodeInvalidSyscallInput)
            else do
              let (_, f1) = frameTakeAuthToken tid f
              if childCount == 0
                then do
                  modifyActiveFrame h (const f1)
                  pure (Right (ConsumeAuthTokenResult tok 0 0))
                else do
                  let start = fromIntegral (frameNextAuthTokenId f1)
                      addChild fr child =
                        case frameAddAuthToken
                          AuthToken {mint_address = tok.mint_address, expiry_height = tok.expiry_height, root_node = child}
                          fr of
                          Just (_, fr') -> fr'
                          Nothing -> fr
                      f2 = foldl addChild f1 children
                  modifyActiveFrame h (const f2)
                  pure (Right (ConsumeAuthTokenResult tok start (fromIntegral childCount)))

checkAuthTokensOwned :: SystemCallHandler -> [Word32] -> IO Bool
checkAuthTokensOwned h ids = do
  f <- activeFrame h
  pure (all (`frameCheckOwnsAuthToken` f) ids)

-- | Whether the active frame's parent has ids left for @count@ returned
-- tokens.
checkParentCanTakeAuthTokens :: SystemCallHandler -> Int -> IO Bool
checkParentCanTakeAuthTokens h count =
  readIORef (schFrames h) >>= \case
    (_ : p : _) -> pure (count <= fromIntegral (maxBound :: Word32) - frameNextAuthTokenId p)
    _ -> pure True
