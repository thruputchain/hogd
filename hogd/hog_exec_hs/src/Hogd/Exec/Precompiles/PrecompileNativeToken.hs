-- | The native token precompile (precompiles/precompile_native_token.{h,cc}).
--
-- Implements the ERC20, ERC20Permit and ERC165 traits over the chain's
-- native token balances, byte-identically to the RV32 @itc_erc20@ /
-- @itc_erc20_permit@ reference contracts: same entrypoint ids, calldata and
-- return encodings, ERC20Error reverts and events.  Allowances and the total
-- supply live in the native token's own contract storage; balances are the
-- free native token balance items.
module Hogd.Exec.Precompiles.PrecompileNativeToken
  ( makeNativeTokenContract
  , nativeTokenMethods
  , nativeTokenUncheckedTransfer
    -- * Event signatures
  , nativeTokenTransferEventSignature
  , nativeTokenAllowanceChangeEventSignature
  , nativeTokenMintEventSignature
  , nativeTokenAllowanceDeleteEventSignature
    -- * Event bodies
  , transferEventBytes
  , allowanceChangeEventBytes
  , mintEventBytes
  , allowanceDeleteEventBytes
    -- * Internals exposed for tests
  , allowanceKeyPreimage
  , nativeTokenImplementedInterfaces
  , kSupportsInterfaceGas
  , kSupportsInterfacesGas
  ) where

import qualified Data.ByteString as B
import Data.ByteString.Builder (Builder)
import qualified Data.ByteString.Builder as BB
import Data.Word
import Hogd.BuiltContracts.Codec
import Hogd.BuiltContracts.ItcErc20
import Hogd.Exec.Common.Int128
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Log
import Hogd.Exec.Precompiles.NativeContractBuilder
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.ContractTraitClient (nscSyscall)
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallEntry (hostOutSpan)
import Hogd.Exec.Runtime.Syscalls
import Hogd.Exec.Runtime.Types (MethodID)
import Hogd.Genesis.Constants (kGenesisRootContractAddress, kNativeTokenTotalSupplyKey)
import Hogd.HogEvent.Utilities
import Hogd.Xdr

------------------------------------------------------------------------------
-- Event signatures and bodies

-- | The SDK's event signatures for the @thruput-erc20-if@ crate (0.1.x),
-- so the native token's events are byte-identical to @itc_erc20@'s.
nativeTokenTransferEventSignature, nativeTokenAllowanceChangeEventSignature, nativeTokenMintEventSignature, nativeTokenAllowanceDeleteEventSignature :: Topic
nativeTokenTransferEventSignature = computeEventSignature "0.1::thruput-erc20-if::TransferEvent"
nativeTokenAllowanceChangeEventSignature = computeEventSignature "0.1::thruput-erc20-if::AllowanceChangeEvent"
nativeTokenMintEventSignature = computeEventSignature "0.1::thruput-erc20-if::MintEvent"
nativeTokenAllowanceDeleteEventSignature = computeEventSignature "0.1::thruput-erc20-if::AllowanceDeleteEvent"

int128Cxs :: Integer -> Builder
int128Cxs = cCxsPut i128C

-- | @TransferEvent { from_address, to_address, amount }@ (3 topics).
transferEventBytes :: ContractAddress -> ContractAddress -> Integer -> B.ByteString
transferEventBytes from to amount =
  makeEventFromCxsStruct nativeTokenTransferEventSignature (BB.byteString from <> BB.byteString to <> int128Cxs amount)

-- | @NativeTokenAllowanceChangeEvent { owner, spender, difference }@ (3
-- topics).
allowanceChangeEventBytes :: ContractAddress -> ContractAddress -> Integer -> B.ByteString
allowanceChangeEventBytes owner spender diff =
  makeEventFromCxsStruct nativeTokenAllowanceChangeEventSignature (BB.byteString owner <> BB.byteString spender <> int128Cxs diff)

-- | @NativeTokenMintEvent { to_address, amount }@ (2 topics).
mintEventBytes :: ContractAddress -> Integer -> B.ByteString
mintEventBytes to amount =
  makeEventFromCxsStruct nativeTokenMintEventSignature (BB.byteString to <> int128Cxs amount)

-- | @NativeTokenAllowanceDeleteEvent { owner, spender, removed }@ (3
-- topics).
allowanceDeleteEventBytes :: ContractAddress -> ContractAddress -> Integer -> B.ByteString
allowanceDeleteEventBytes owner spender removed =
  makeEventFromCxsStruct nativeTokenAllowanceDeleteEventSignature (BB.byteString owner <> BB.byteString spender <> int128Cxs removed)

------------------------------------------------------------------------------
-- Syscall helpers

-- | @emit_event<NumTopics>@: split the leading topics off the event buffer
-- and log it through kLogEvent.
emitEvent :: NativeSyscallContext -> Int -> B.ByteString -> String -> IO (InvokeStatus ())
emitEvent ctx numTopics buf name = do
  checkM (numTopics <= 3 && B.length buf >= numTopics * 32) "emit_event: event shorter than its topics"
  let topics = [B.take 32 (B.drop (32 * i) buf) | i <- [0 .. numTopics - 1]]
      topic i = if i < numTopics then Just (topics !! i) else Nothing
      dat = B.drop (numTopics * 32) buf
  (r, ()) <- nscSyscall ctx KLogEvent () $ \_ -> sysLogEvent (nscHandler ctx) (topic 0) (topic 1) (topic 2) dat
  case r of
    Left e -> do
      left <- nscGasRemaining ctx
      logInfo
        ( "native token " ++ name ++ " event: kLogEvent failed: " ++ show e ++ " (" ++ show numTopics ++ " topics, "
            ++ show (B.length dat) ++ " data bytes, " ++ show left ++ " gas left)"
        )
      Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> do
          logInfo
            ( "native token " ++ name ++ " event: kLogEvent rejected the event: " ++ show code ++ " (" ++ show numTopics
                ++ " topics, " ++ show (B.length dat) ++ " data bytes)"
            )
          pure (Left Deterministic)
      | otherwise -> pure (Right ())

-- | The caller, via kGetMsgSender.
getMsgSender' :: NativeSyscallContext -> IO (InvokeStatus ContractAddress)
getMsgSender' ctx = do
  (r, sender) <- nscSyscall ctx KGetMsgSender zeroAddress $ \_ -> sysGetMsgSender (nscHandler ctx)
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> pure (Left Deterministic)
      | otherwise -> pure (Right sender)

zeroAddress :: ContractAddress
zeroAddress = B.replicate 32 0

-- | The TXS encoding of (owner, spender) the allowance key hashes.
allowanceKeyPreimage :: ContractAddress -> ContractAddress -> B.ByteString
allowanceKeyPreimage owner spender = encodeC Txs (pairC addressC addressC) (owner, spender)

-- | @allowance_key@: sha256 of the preimage, through kSHA256 so the digest
-- is priced as a syscall.
allowanceKey :: NativeSyscallContext -> ContractAddress -> ContractAddress -> IO (InvokeStatus StorageKeyContractData)
allowanceKey ctx owner spender = do
  let dat = allowanceKeyPreimage owner spender
  (r, key) <- nscSyscall ctx KSHA256 zeroAddress $ \g -> sysSHA256 (nscHandler ctx) g dat
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> do
          logError ("Unrecoverable: kSHA256 returned " ++ show code)
          pure (Left InvokeErrorCode_Unrecoverable)
      | otherwise -> pure (Right key)

-- | kNonnegativeIntGet: (status, (set value, set present, add value)).
nnintGet :: NativeSyscallContext -> StorageKeyContractData -> Bool -> IO (HostFnStatus SyscallErrorCode, (Integer, Bool, Integer))
nnintGet ctx key force = nscSyscall ctx KNonnegativeIntGet (0, False, 0) $ \_ -> sysNonnegativeIntGet (nscHandler ctx) key force

nnintSetAdd :: NativeSyscallContext -> StorageKeyContractData -> Maybe Integer -> Integer -> IO (HostFnStatus SyscallErrorCode)
nnintSetAdd ctx key set add = fst <$> nscSyscall ctx KNonnegativeIntSetAdd () (\_ -> sysNonnegativeIntSetAdd (nscHandler ctx) key set add)

erc20Err :: ERC20Error -> Result Erc20Exn a
erc20Err = Left . ErrException . Erc20Exn

-- | Strict allowance adjustment (the SDK's non_negative_int_try_strict_inc
-- as itc_erc20::add_to_allowance uses it): force-load the committed value
-- and reject an out-of-range running total at invoke time.  Right False is
-- the caller's TooLarge.
adjustAllowance :: NativeSyscallContext -> ContractAddress -> ContractAddress -> Integer -> IO (InvokeStatus Bool)
adjustAllowance ctx owner spender amount = do
  keyR <- allowanceKey ctx owner spender
  case keyR of
    Left e -> pure (Left e)
    Right key -> do
      (r, (set0, present0, add0)) <- nnintGet ctx key True
      let proceed set present add
            | addOverflows128 add amount = pure (Right False)
            | otherwise = do
                let newAdd = add + amount
                if addOverflows128 set newAdd || set + newAdd < 0
                  then pure (Right False)
                  else do
                    r2 <- nnintSetAdd ctx key (if present then Just set else Nothing) newAdd
                    case r2 of
                      Left e -> Left <$> hostFnErrorConversion e
                      Right code -> pure (Right (code == KSyscallErrorCodeNone))
      case r of
        Left e -> Left <$> hostFnErrorConversion e
        Right KSyscallErrorCodeStorageKeyNotExist -> proceed 0 True 0
        Right code
          | code /= KSyscallErrorCodeNone -> pure (Left Deterministic)
          | otherwise -> proceed set0 present0 add0

-- | A balance read: a staged-negative balance (which the RV32 reference can
-- never hold) is a deterministic fault.
readBalance :: NativeSyscallContext -> ContractAddress -> IO (InvokeStatus Integer)
readBalance ctx addr = do
  r <- nscNativeTokenBalance ctx addr
  pure $ case r of
    Left e -> Left (systemToInvoke e)
    Right (Left _) -> Left Deterministic
    Right (Right b) -> Right b

modifyBalance :: NativeSyscallContext -> ContractAddress -> Integer -> IO (InvokeStatus ())
modifyBalance ctx addr amount = do
  r <- nscModifyNativeTokenBalance ctx addr amount
  pure $ case r of
    Left e -> Left (systemToInvoke e)
    Right (Left _) -> Left Deterministic
    Right (Right ()) -> Right ()

-- | @native_token_unchecked_transfer@: debit @from@, credit @to@; no
-- balance check and no event.  @amount@ must be nonnegative.
nativeTokenUncheckedTransfer :: NativeSyscallContext -> ContractAddress -> ContractAddress -> Integer -> IO (InvokeStatus ())
nativeTokenUncheckedTransfer ctx from to amount = do
  checkM (amount >= 0) "native_token_unchecked_transfer: negative amount"
  r1 <- modifyBalance ctx from (negate amount)
  case r1 of
    Left e -> pure (Left e)
    Right () -> modifyBalance ctx to amount

-- | @add_to_total_supply@: the plain (non-strict) nnint increment; the only
-- invoke-time failure is an overflow of the in-transaction add, reported as
-- the SDK's NonNegativeIntOverflow library abort.  Nothing once staged.
addToTotalSupply :: NativeSyscallContext -> Integer -> IO (Maybe (InvokeStatus (Result Erc20Exn ())))
addToTotalSupply ctx diff = do
  (r, (set, present, add)) <- nnintGet ctx kNativeTokenTotalSupplyKey False
  case r of
    Left e -> Just . Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> pure (Just (Left Deterministic))
      | addOverflows128 add diff -> pure (Just (Right (Left (ErrLibrary LibraryExn_kLibraryExnNonNegativeIntOverflow))))
      | otherwise -> do
          r2 <- nnintSetAdd ctx kNativeTokenTotalSupplyKey (if present then Just set else Nothing) (add + diff)
          case r2 of
            Left e -> Just . Left <$> hostFnErrorConversion e
            Right c
              | c /= KSyscallErrorCodeNone -> pure (Just (Left Deterministic))
              | otherwise -> pure Nothing

-- | Run an action unless the frame is out of gas after charging @n@.
withGas :: NativeSyscallContext -> Word64 -> IO (InvokeStatus a) -> IO (InvokeStatus a)
withGas ctx n act = do
  ok <- nscConsumeGas ctx n
  if ok then act else pure (Left InvokeErrorCode_OutOfGas)

-- | Bind for the 'InvokeStatus' layer.
(>>?) :: IO (InvokeStatus a) -> (a -> IO (InvokeStatus b)) -> IO (InvokeStatus b)
m >>? k = m >>= either (pure . Left) k

infixl 1 >>?

------------------------------------------------------------------------------
-- ERC20 bodies

mintImpl :: NativeSyscallContext -> (ContractAddress, Integer) -> IO (InvokeStatus (Result Erc20Exn ()))
mintImpl ctx (receiver, amount) = withGas ctx 1 $
  getMsgSender' ctx >>? \sender ->
    if sender /= kGenesisRootContractAddress
      then pure (Right (erc20Err MustBeContractOwner))
      else
        if amount < 0
          then pure (Right (erc20Err NegativeAmount))
          else
            readBalance ctx receiver >>? \bal ->
              if addOverflows128 bal amount
                then pure (Right (erc20Err TooLarge))
                else
                  modifyBalance ctx receiver amount >>? \() -> do
                    supply <- addToTotalSupply ctx amount
                    case supply of
                      Just failure -> pure failure
                      Nothing ->
                        emitEvent ctx 2 (mintEventBytes receiver amount) "mint" >>? \() ->
                          pure (Right (Right ()))

-- | @move_balance@: the strict owner debit, the credit and the Transfer
-- event every spending path shares.  @tooLarge@ is the calling
-- interface's TooLarge error.
moveBalance :: NativeSyscallContext -> ContractAddress -> ContractAddress -> Integer -> String -> r -> r -> IO (InvokeStatus r)
moveBalance ctx owner receiver amount entryPoint tooLarge okResult =
  readBalance ctx owner >>? \avail ->
    if amount > avail
      then pure (Right tooLarge)
      else
        nativeTokenUncheckedTransfer ctx owner receiver amount >>? \() ->
          emitEvent ctx 3 (transferEventBytes owner receiver amount) entryPoint >>? \() ->
            pure (Right okResult)

transferImpl :: NativeSyscallContext -> (ContractAddress, Integer) -> IO (InvokeStatus (Result Erc20Exn ()))
transferImpl ctx (receiver, amount) = do
  logTrace ("native token transfer: receiver " ++ showHex' receiver ++ " amount " ++ show amount)
  withGas ctx 1 $
    getMsgSender' ctx >>? \owner ->
      if amount < 0
        then pure (Right (erc20Err NegativeAmount))
        else moveBalance ctx owner receiver amount "transfer" (erc20Err TooLarge) (Right ())

transferFromImpl :: NativeSyscallContext -> (ContractAddress, ContractAddress, Integer) -> IO (InvokeStatus (Result Erc20Exn ()))
transferFromImpl ctx (owner, receiver, amount) = do
  logTrace ("native token transfer_from: owner " ++ showHex' owner ++ " receiver " ++ showHex' receiver ++ " amount " ++ show amount)
  withGas ctx 1 $
    getMsgSender' ctx >>? \spender ->
      if amount < 0
        then pure (Right (erc20Err NegativeAmount))
        else do
          allowed <-
            if owner /= spender
              then adjustAllowance ctx owner spender (negate amount)
              else pure (Right True)
          case allowed of
            Left e -> pure (Left e)
            Right False -> pure (Right (erc20Err TooLarge))
            Right True -> moveBalance ctx owner receiver amount "transfer_from" (erc20Err TooLarge) (Right ())

emitAllowanceChange :: NativeSyscallContext -> ContractAddress -> ContractAddress -> Integer -> IO (InvokeStatus ())
emitAllowanceChange ctx owner spender diff = emitEvent ctx 3 (allowanceChangeEventBytes owner spender diff) "allowance change"

adjustAllowanceImpl :: NativeSyscallContext -> (ContractAddress, Integer) -> IO (InvokeStatus (Result Erc20Exn ()))
adjustAllowanceImpl ctx (spender, amount) = do
  logTrace ("native token adjust_allowance: spender " ++ showHex' spender ++ " amount " ++ show amount)
  withGas ctx 1 $
    getMsgSender' ctx >>? \owner ->
      adjustAllowance ctx owner spender amount >>? \case
        False -> pure (Right (erc20Err TooLarge))
        True -> emitAllowanceChange ctx owner spender amount >>? \() -> pure (Right (Right ()))

-- | @read_allowance@: the committed base plus the in-transaction delta; a
-- never-written allowance reads zero.
readAllowance :: NativeSyscallContext -> StorageKeyContractData -> IO (InvokeStatus Integer)
readAllowance ctx key = do
  (r, (base, present, delta)) <- nnintGet ctx key True
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right KSyscallErrorCodeStorageKeyNotExist -> pure (Right 0)
    Right code
      | code /= KSyscallErrorCodeNone || not present || addOverflows128 base delta || base + delta < 0 -> pure (Left Deterministic)
      | otherwise -> pure (Right (base + delta))

getAllowanceImpl :: NativeSyscallContext -> (ContractAddress, ContractAddress) -> IO (InvokeStatus (Result Erc20Exn Integer))
getAllowanceImpl ctx (owner, spender) = withGas ctx 1 $
  allowanceKey ctx owner spender >>? \key ->
    readAllowance ctx key >>? \remaining -> pure (Right (Right remaining))

-- | @delete_allowance@: delete under an exclusive lock and emit
-- AllowanceDeleteEvent with the amount removed.
deleteAllowance :: NativeSyscallContext -> ContractAddress -> ContractAddress -> IO (InvokeStatus ())
deleteAllowance ctx owner spender =
  allowanceKey ctx owner spender >>? \key ->
    readAllowance ctx key >>? \remaining -> do
      (d, ()) <- nscSyscall ctx KDeleteObject () $ \_ -> sysDeleteObject (nscHandler ctx) key
      case d of
        Left e -> Left <$> hostFnErrorConversion e
        Right code | code /= KSyscallErrorCodeNone -> pure (Left Deterministic)
        Right _ -> do
          (l, ()) <- nscSyscall ctx KSetStorageFlags () $ \_ -> sysSetStorageFlags (nscHandler ctx) key exclusiveLock
          case l of
            Left e -> Left <$> hostFnErrorConversion e
            Right code
              | code /= KSyscallErrorCodeNone -> do
                  logError ("Unrecoverable: kSetStorageFlags after allowance deletion returned " ++ show code)
                  pure (Left InvokeErrorCode_Unrecoverable)
              | otherwise -> emitEvent ctx 3 (allowanceDeleteEventBytes owner spender remaining) "allowance delete"
  where
    exclusiveLock = let StorageDeltaFlags f = ExclusiveLock in fromIntegral f

deleteAllowanceImpl :: NativeSyscallContext -> ContractAddress -> IO (InvokeStatus (Result Erc20Exn ()))
deleteAllowanceImpl ctx spender = withGas ctx 1 $
  getMsgSender' ctx >>? \owner ->
    deleteAllowance ctx owner spender >>? \() -> pure (Right (Right ()))

getBalanceOfImpl :: NativeSyscallContext -> ContractAddress -> IO (InvokeStatus (Result Erc20Exn Integer))
getBalanceOfImpl ctx addr = do
  logTrace ("native token get_balance_of: contract_address " ++ showHex' addr)
  withGas ctx 1 $ readBalance ctx addr >>? \b -> pure (Right (Right b))

getBalanceImpl :: NativeSyscallContext -> () -> IO (InvokeStatus (Result Erc20Exn Integer))
getBalanceImpl ctx () = do
  logTrace "native token get_balance"
  withGas ctx 1 $
    getMsgSender' ctx >>? \caller ->
      readBalance ctx caller >>? \b -> pure (Right (Right b))

totalSupplyImpl :: NativeSyscallContext -> () -> IO (InvokeStatus (Result Erc20Exn Integer))
totalSupplyImpl ctx () = do
  logTrace "native token total_supply"
  withGas ctx 1 $ do
    (r, (base, present, add)) <- nnintGet ctx kNativeTokenTotalSupplyKey True
    case r of
      Left e -> Left <$> hostFnErrorConversion e
      Right KSyscallErrorCodeStorageKeyNotExist -> pure (Right (Right 0))
      Right code
        | code /= KSyscallErrorCodeNone || not present || addOverflows128 base add || base + add < 0 -> pure (Left Deterministic)
        | otherwise -> pure (Right (Right (base + add)))

------------------------------------------------------------------------------
-- ERC20Permit bodies

-- | Consume a grant token as the SDK's @Signed<Grant>::consume@ does:
-- Right (Left exn) is the library exception the SDK would throw instead.
consumeGrant :: NativeSyscallContext -> Codec g -> SignedToken -> IO (InvokeStatus (Either LibraryExn (g, ContractAddress)))
consumeGrant ctx grantC (SignedToken handle) = do
  (out, readOut) <- hostOutSpan kAuthTokenMaxLen
  (r, (minter, payloadLen, _childStart, childCount)) <-
    nscSyscall ctx KConsumeAuthToken (zeroAddress, 0, 0, 0) $ \_ -> sysConsumeAuthToken (nscHandler ctx) handle out
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> pure (Right (Left (syscallException KConsumeAuthToken code)))
      | payloadLen > kAuthTokenMaxLen -> do
          logError
            ( "Unrecoverable: kConsumeAuthToken reported " ++ show payloadLen ++ " payload bytes into a "
                ++ show (kAuthTokenMaxLen :: Int) ++ "-byte buffer"
            )
          pure (Left InvokeErrorCode_Unrecoverable)
      | otherwise -> do
          payload <- B.take (fromIntegral payloadLen) <$> readOut
          pure . Right $ case decodeC Cxs grantC payload of
            Nothing -> Left (LibraryExn_auth_token AuthTokenError_kAuthTokenConsumeDecodeFailure)
            Just grant
              | childCount /= 0 -> Left (LibraryExn_auth_token AuthTokenError_kAuthTokenReceiveAuthHandlesFailure)
              | otherwise -> Right (grant, minter)

permitErr :: ERC20PermitError -> Result PermitExn a
permitErr = Left . ErrException . PermitErr

permitErc20Err :: ERC20Error -> Result PermitExn a
permitErc20Err = Left . ErrException . PermitErc20Err

transferFromSignedImpl :: NativeSyscallContext -> (SignedToken, ContractAddress, Integer) -> IO (InvokeStatus (Result PermitExn ()))
transferFromSignedImpl ctx (token, receiver, requested) = do
  logTrace ("native token transfer_from_signed: handle " ++ show (stHandle token) ++ " receiver " ++ showHex' receiver ++ " amount " ++ show requested)
  withGas ctx 1 $
    consumeGrant ctx permitGrantC token >>? \case
      Left exn -> pure (Right (Left (ErrLibrary exn)))
      Right (grant, owner) ->
        getMsgSender' ctx >>? \caller ->
          if caller /= pgSpender grant
            then pure (Right (permitErr NotSpender))
            else
              if requested > pgAmount grant
                then pure (Right (permitErr ExceedsGrant))
                else
                  if requested < 0
                    then pure (Right (permitErc20Err NegativeAmount))
                    else moveBalance ctx owner receiver requested "transfer_from_signed" (permitErc20Err TooLarge) (Right ())

increaseAllowanceSignedImpl :: NativeSyscallContext -> SignedToken -> IO (InvokeStatus (Result PermitExn ()))
increaseAllowanceSignedImpl ctx token = do
  logTrace ("native token increase_allowance_signed: handle " ++ show (stHandle token))
  withGas ctx 1 $
    consumeGrant ctx permitGrantC token >>? \case
      Left exn -> pure (Right (Left (ErrLibrary exn)))
      Right (grant, owner) -> do
        let diff = pgAmount grant
        if diff < 0
          then pure (Right (permitErc20Err NegativeAmount))
          else
            adjustAllowance ctx owner (pgSpender grant) diff >>? \case
              False -> pure (Right (permitErc20Err TooLarge))
              True -> emitAllowanceChange ctx owner (pgSpender grant) diff >>? \() -> pure (Right (Right ()))

deleteAllowanceSignedImpl :: NativeSyscallContext -> SignedToken -> IO (InvokeStatus (Result PermitExn ()))
deleteAllowanceSignedImpl ctx token = do
  logTrace ("native token delete_allowance_signed: handle " ++ show (stHandle token))
  withGas ctx 1 $
    consumeGrant ctx deleteAllowanceGrantC token >>? \case
      Left exn -> pure (Right (Left (ErrLibrary exn)))
      Right (grant, owner) ->
        deleteAllowance ctx owner (dagSpender grant) >>? \() -> pure (Right (Right ()))

------------------------------------------------------------------------------
-- ERC165

-- | The interfaces 'makeNativeTokenContract' registers.
nativeTokenImplementedInterfaces :: [InterfaceId]
nativeTokenImplementedInterfaces = map traitInterfaceId [erc20Trait, erc20PermitTrait, erc165Trait]

-- | Ballparks of what the RV32 reference's frame consumes.
kSupportsInterfaceGas, kSupportsInterfacesGas :: Word64
kSupportsInterfaceGas = 500
kSupportsInterfacesGas = 2500

supportsInterfaceImpl :: NativeSyscallContext -> InterfaceId -> IO (InvokeStatus Bool)
supportsInterfaceImpl ctx iid = withGas ctx kSupportsInterfaceGas $ pure (Right (iid `elem` nativeTokenImplementedInterfaces))

supportsInterfacesImpl :: NativeSyscallContext -> [InterfaceId] -> IO (InvokeStatus [Bool])
supportsInterfacesImpl ctx ids = withGas ctx kSupportsInterfacesGas $ pure (Right (map (`elem` nativeTokenImplementedInterfaces) ids))

------------------------------------------------------------------------------
-- The contract

erc20Result :: (a -> (Builder, Builder)) -> IO (InvokeStatus (Result Erc20Exn a)) -> IO (InvokeStatus MethodReturn)
erc20Result enc act = fmap (returnResult Cxs enc) <$> act

permitResult :: IO (InvokeStatus (Result PermitExn ())) -> IO (InvokeStatus MethodReturn)
permitResult act = fmap (returnResult Cxs (codecPair unitC)) <$> act

-- | The method table of 'makeNativeTokenContract'.
nativeTokenMethods :: [(MethodID, NativeFunction)]
nativeTokenMethods =
  [ traitAlwaysErrorMethod erc20Trait "initialize" (argSpec addressC) 1
  , traitMethod erc20Trait "transfer" (argSpec (pairC addressC i128C)) $ \ctx a -> erc20Result unit (transferImpl ctx a)
  , traitMethod erc20Trait "transfer_from" (argSpec (tripleC addressC addressC i128C)) $ \ctx a -> erc20Result unit (transferFromImpl ctx a)
  , traitMethod erc20Trait "allowance" (argSpec (pairC addressC addressC)) $ \ctx a -> erc20Result amount (getAllowanceImpl ctx a)
  , traitMethod erc20Trait "adjust_allowance" (argSpec (pairC addressC i128C)) $ \ctx a -> erc20Result unit (adjustAllowanceImpl ctx a)
  , traitMethod erc20Trait "delete_allowance" (argSpec addressC) $ \ctx a -> erc20Result unit (deleteAllowanceImpl ctx a)
  , -- Native account balances also support storage accounting; their
    -- database path does not support deletion or exclusive locks.
    traitAlwaysErrorMethod erc20Trait "delete_balance" noArgs 1
  , traitMethod erc20Trait "get_balance_of" (argSpec addressC) $ \ctx a -> erc20Result amount (getBalanceOfImpl ctx a)
  , traitMethod erc20Trait "get_balance" noArgs $ \ctx a -> erc20Result amount (getBalanceImpl ctx a)
  , traitMethod erc20Trait "mint" (argSpec (pairC addressC i128C)) $ \ctx a -> erc20Result unit (mintImpl ctx a)
  , traitMethod erc20Trait "total_supply" noArgs $ \ctx a -> erc20Result amount (totalSupplyImpl ctx a)
  , traitMethod erc20PermitTrait "transfer_from_signed" transferFromSignedArgs $ \ctx a -> permitResult (transferFromSignedImpl ctx a)
  , traitMethod erc20PermitTrait "increase_allowance_signed" signedOnlyArgs $ \ctx a -> permitResult (increaseAllowanceSignedImpl ctx a)
  , traitMethod erc20PermitTrait "delete_allowance_signed" signedOnlyArgs $ \ctx a -> permitResult (deleteAllowanceSignedImpl ctx a)
  , traitMethod erc165Trait "supports_interface" (argSpec interfaceIdC) $ \ctx a ->
      fmap (returnValue Cxs . codecPair boolC) <$> supportsInterfaceImpl ctx a
  , traitMethod erc165Trait "supports_interfaces" (argSpec interfaceIdsC) $ \ctx a ->
      fmap (returnValue Cxs . codecPair interfaceSupportC) <$> supportsInterfacesImpl ctx a
  ]
  where
    unit = codecPair unitC
    amount = codecPair i128C
    transferFromSignedArgs =
      argSpecSigned (tripleC signedTokenC addressC i128C) 1 $ \case
        [t] -> \(_, r, a) -> (t, r, a)
        _ -> id
    signedOnlyArgs = argSpecSigned signedTokenC 1 $ \case
      [t] -> const t
      _ -> id

-- | @make_native_token_contract@.
makeNativeTokenContract :: NativeContract
makeNativeTokenContract
  | all (`checkMethods` nativeTokenMethods) [erc20Trait, erc20PermitTrait, erc165Trait] = buildNativeContract nativeTokenMethods
  | otherwise = error "native token is missing an entrypoint"

showHex' :: B.ByteString -> String
showHex' = concatMap (\w -> let s = "0123456789abcdef" in [s !! fromIntegral (w `div` 16), s !! fromIntegral (w `mod` 16)]) . B.unpack
