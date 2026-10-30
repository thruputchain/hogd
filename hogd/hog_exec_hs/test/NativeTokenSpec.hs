-- | The native token precompile driven through the executor against the
-- in-memory ledger: ports of native_token_allowance_test.cc and the native
-- side of erc20_parity_test.cc / native_rv32_token_event_parity_test.cc
-- (the RV32 reference is not available here, so the expected bytes are the
-- reference's, written out).
--
-- A forwarder contract deployed at the actor's address makes each call, so
-- the actor is the token's msg_sender; its return buffer records the
-- token's frame status and return or abort bytes.
module NativeTokenSpec (spec) where

import qualified Data.ByteString as B
import Data.Word (Word64)
import Harness.Env
import Harness.NativeContracts
import Hogd.BuiltContracts.Codec
import Hogd.BuiltContracts.ItcErc20
import Hogd.Crypto (sha256)
import Hogd.Exec.Common.Int128 (int128Max)
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Precompiles.NativeContractBuilder (decodingArgsAbortBytes)
import Hogd.Exec.Precompiles.PrecompileNativeToken
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallHandler (setReturnBuffer)
import Hogd.Exec.Runtime.Types (RuntimeContext)
import Hogd.Genesis.Constants
import Hogd.Xdr
import Test.Hspec

kForwarderId :: Num a => a
kForwarderId = 21

alice, bob, carol :: ContractAddress
alice = contractAddress 170
bob = contractAddress 171
carol = contractAddress 172

token :: ContractAddress
token = kNativeTokenContractAddress

ctx0 :: RuntimeContext
ctx0 = nativeTestContext [(kForwarderId, forwarder)]

-- | Deploy forwarders at the actors (and at the genesis root, the token's
-- mint authority).
setupActors :: TestEnv -> IO ()
setupActors env = mapM_ (\a -> deployContract env a (nativeContractXdr kForwarderId)) [alice, bob, carol, kGenesisRootContractAddress]

-- | One transaction: @caller@ calls the token's @method@; then the block
-- is closed.
call :: TestEnv -> ContractAddress -> ContractTrait -> B.ByteString -> Calldata -> IO (TxRun, Record)
call env caller trait method cd = callWith env ctx0 caller (traitMethodId trait method) cd

callWith :: TestEnv -> RuntimeContext -> ContractAddress -> MethodID' -> Calldata -> IO (TxRun, Record)
callWith env rc caller method cd = do
  tx <- rootTransaction env caller 0 (forwarderCalldata token method cd)
  run <- runTx env rc tx
  advanceBlock env
  let rec = case trOutput run of
        InvocationFrameOutput_return_buffer b -> parseRecord b
        _ -> RecMalformed B.empty
  pure (run, rec)

type MethodID' = Word64

erc20 :: B.ByteString -> MethodID'
erc20 = traitMethodId erc20Trait

amountC :: Integer -> B.ByteString
amountC = encodeC Cxs i128C

addrAmount :: ContractAddress -> Integer -> Calldata
addrAmount a n = encodeC Cxs (pairC addressC i128C) (a, n)

returned :: B.ByteString -> Record
returned = RecFrame InvocationFrameStatusCode_Return

aborted :: B.ByteString -> Record
aborted = RecFrame Abort

erc20Abort :: ERC20Error -> Record
erc20Abort e = aborted (serializeContractError (ErrException (Erc20Exn e)))

permitAbort :: PermitExn -> Record
permitAbort e = aborted (serializeContractError (ErrException e))

faulted :: Record
faulted = RecFrame Fault (B.singleton 0xFF)

ok :: TxRun -> Expectation
ok run = do
  irResult (trRoot run) `shouldBe` Right ()
  trCommit run `shouldBe` ExecResultOK

events :: TxRun -> [TxEmittedEvent]
events run = maybe [] (\l -> l.events) (trEventLog run)

tokenEvent :: Word -> B.ByteString -> TxEmittedEvent
tokenEvent n bytes = TxEmittedEvent {contract_address = token, num_topics = fromIntegral n, serialized_event = bytes}

allowanceKeyOf :: ContractAddress -> ContractAddress -> AddressAndKey
allowanceKeyOf o s = contractDataKey token (sha256 (o <> s))

allowanceOf :: TestEnv -> ContractAddress -> ContractAddress -> IO Record
allowanceOf env o s = snd <$> call env alice erc20Trait "allowance" (encodeC Cxs (pairC addressC addressC) (o, s))

spec :: Spec
spec = around withTestEnv $ do
  allowanceSpec
  transferSpec
  mintSpec
  querySpec
  deleteSpec
  dispatchSpec
  permitSpec
  erc165Spec

allowanceSpec :: SpecWith TestEnv
allowanceSpec = describe "allowances" $ do
  it "reads a fresh pair's allowance as zero" $ \env -> do
    setupActors env
    (run, rec) <- call env alice erc20Trait "allowance" (encodeC Cxs (pairC addressC addressC) (alice, bob))
    ok run
    rec `shouldBe` returned (amountC 0)
    events run `shouldBe` []

  it "adjusts a fresh allowance from zero and reads it back" $ \env -> do
    setupActors env
    (run, rec) <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 500)
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (allowanceChangeEventBytes alice bob 500)]
    lookupItem env (allowanceKeyOf alice bob) `shouldReturn` Just (WireStorageItem_nnint (X_int128_xdr 0 500))
    allowanceOf env alice bob `shouldReturn` returned (amountC 500)

  it "accumulates allowance across adjusts" $ \env -> do
    setupActors env
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 100)
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 50)
    allowanceOf env alice bob `shouldReturn` returned (amountC 150)

  it "refuses a negative adjust that would drive the allowance below zero" $ \env -> do
    setupActors env
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 100)
    (run, rec) <- call env alice erc20Trait "adjust_allowance" (addrAmount bob (-101))
    ok run
    rec `shouldBe` erc20Abort TooLarge
    events run `shouldBe` []
    (_, rec2) <- call env alice erc20Trait "adjust_allowance" (addrAmount bob (-100))
    rec2 `shouldBe` returned ""
    allowanceOf env alice bob `shouldReturn` returned (amountC 0)

  it "rejects an overflowing adjust at invoke" $ \env -> do
    setupActors env
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob int128Max)
    (_, rec) <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 1)
    rec `shouldBe` erc20Abort TooLarge
    allowanceOf env alice bob `shouldReturn` returned (amountC int128Max)

  it "allows adjusting the allowance for oneself" $ \env -> do
    setupActors env
    (_, rec) <- call env alice erc20Trait "adjust_allowance" (addrAmount alice 7)
    rec `shouldBe` returned ""
    allowanceOf env alice alice `shouldReturn` returned (amountC 7)

  it "fails transfer_from without an allowance" $ \env -> do
    setupActors env
    grantNativeTokens env alice 1000
    (run, rec) <- call env bob erc20Trait "transfer_from" (encodeC Cxs (tripleC addressC addressC i128C) (alice, carol, 10))
    ok run
    rec `shouldBe` erc20Abort TooLarge
    nativeBalance env alice `shouldReturn` 1000
    nativeBalance env carol `shouldReturn` 0

  it "deducts transfer_from from the allowance and moves the balance" $ \env -> do
    setupActors env
    grantNativeTokens env alice 1000
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 300)
    (run, rec) <- call env bob erc20Trait "transfer_from" (encodeC Cxs (tripleC addressC addressC i128C) (alice, carol, 200))
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (transferEventBytes alice carol 200)]
    nativeBalance env alice `shouldReturn` 800
    nativeBalance env carol `shouldReturn` 200
    allowanceOf env alice bob `shouldReturn` returned (amountC 100)

  it "reverts the allowance debit when the owner's balance is short" $ \env -> do
    setupActors env
    grantNativeTokens env alice 50
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 300)
    (_, rec) <- call env bob erc20Trait "transfer_from" (encodeC Cxs (tripleC addressC addressC i128C) (alice, carol, 51))
    rec `shouldBe` erc20Abort TooLarge
    allowanceOf env alice bob `shouldReturn` returned (amountC 300)
    nativeBalance env alice `shouldReturn` 50

  it "skips the allowance for a self transfer_from" $ \env -> do
    setupActors env
    grantNativeTokens env alice 100
    (run, rec) <- call env alice erc20Trait "transfer_from" (encodeC Cxs (tripleC addressC addressC i128C) (alice, bob, 40))
    ok run
    rec `shouldBe` returned ""
    lookupItem env (allowanceKeyOf alice alice) `shouldReturn` Nothing
    nativeBalance env bob `shouldReturn` 40

  it "rejects a negative transfer_from amount" $ \env -> do
    setupActors env
    (_, rec) <- call env alice erc20Trait "transfer_from" (encodeC Cxs (tripleC addressC addressC i128C) (alice, bob, -1))
    rec `shouldBe` erc20Abort NegativeAmount

transferSpec :: SpecWith TestEnv
transferSpec = describe "transfer" $ do
  it "moves a funded balance and emits Transfer" $ \env -> do
    setupActors env
    grantNativeTokens env alice 1000
    (run, rec) <- call env alice erc20Trait "transfer" (addrAmount bob 400)
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (transferEventBytes alice bob 400)]
    nativeBalance env alice `shouldReturn` 600
    nativeBalance env bob `shouldReturn` 400

  it "accepts a transfer of the exact balance" $ \env -> do
    setupActors env
    grantNativeTokens env alice 1000
    (_, rec) <- call env alice erc20Trait "transfer" (addrAmount bob 1000)
    rec `shouldBe` returned ""
    nativeBalance env alice `shouldReturn` 0

  it "rejects one over the balance with TooLarge" $ \env -> do
    setupActors env
    grantNativeTokens env alice 1000
    (run, rec) <- call env alice erc20Trait "transfer" (addrAmount bob 1001)
    ok run
    rec `shouldBe` erc20Abort TooLarge
    events run `shouldBe` []
    nativeBalance env alice `shouldReturn` 1000

  it "rejects an over-balance self transfer" $ \env -> do
    setupActors env
    grantNativeTokens env alice 10
    (_, rec) <- call env alice erc20Trait "transfer" (addrAmount alice 11)
    rec `shouldBe` erc20Abort TooLarge

  it "rejects a negative amount with NegativeAmount" $ \env -> do
    setupActors env
    grantNativeTokens env alice 10
    (_, rec) <- call env alice erc20Trait "transfer" (addrAmount bob (-1))
    rec `shouldBe` erc20Abort NegativeAmount

  it "accepts a zero transfer and still emits Transfer" $ \env -> do
    setupActors env
    (run, rec) <- call env alice erc20Trait "transfer" (addrAmount bob 0)
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (transferEventBytes alice bob 0)]

mintSpec :: SpecWith TestEnv
mintSpec = describe "mint" $ do
  it "mints from the genesis root, updating balance and supply" $ \env -> do
    setupActors env
    (run, rec) <- call env kGenesisRootContractAddress erc20Trait "mint" (addrAmount alice 10000000)
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 2 (mintEventBytes alice 10000000)]
    nativeBalance env alice `shouldReturn` 10000000
    (_, supply) <- call env bob erc20Trait "total_supply" ""
    supply `shouldBe` returned (amountC 10000000)
    _ <- call env kGenesisRootContractAddress erc20Trait "mint" (addrAmount bob 5)
    (_, supply2) <- call env bob erc20Trait "total_supply" ""
    supply2 `shouldBe` returned (amountC 10000005)

  it "accepts a zero mint" $ \env -> do
    setupActors env
    (run, rec) <- call env kGenesisRootContractAddress erc20Trait "mint" (addrAmount alice 0)
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 2 (mintEventBytes alice 0)]

  it "rejects mint from anyone but the genesis root" $ \env -> do
    setupActors env
    (_, rec) <- call env alice erc20Trait "mint" (addrAmount alice 1)
    rec `shouldBe` erc20Abort MustBeContractOwner

  it "rejects a negative mint" $ \env -> do
    setupActors env
    (_, rec) <- call env kGenesisRootContractAddress erc20Trait "mint" (addrAmount alice (-5))
    rec `shouldBe` erc20Abort NegativeAmount

  it "rejects a credit past INT128_MAX with TooLarge" $ \env -> do
    setupActors env
    grantNativeTokens env alice int128Max
    (_, rec) <- call env kGenesisRootContractAddress erc20Trait "mint" (addrAmount alice 1)
    rec `shouldBe` erc20Abort TooLarge

  it "throws NonNegativeIntOverflow when the in-transaction supply add overflows" $ \env -> do
    -- Two mints in one transaction whose total exceeds INT128_MAX.
    let twice = singleMethod $ \ctx -> do
          _ <- invokeRecord ctx token (erc20 "mint") (addrAmount alice int128Max) []
          rec <- invokeRecord ctx token (erc20 "mint") (addrAmount bob 1) []
          setReturnBuffer (nscHandler ctx) rec False []
          pure (Right ())
        rc = nativeTestContext [(kForwarderId, twice)]
    deployContract env kGenesisRootContractAddress (nativeContractXdr kForwarderId)
    tx <- rootTransaction env kGenesisRootContractAddress 0 ""
    run <- runTx env rc tx
    case trOutput run of
      InvocationFrameOutput_return_buffer b ->
        parseRecord b `shouldBe` aborted (libraryExceptionBytes LibraryExn_kLibraryExnNonNegativeIntOverflow)
      other -> expectationFailure (show other)

querySpec :: SpecWith TestEnv
querySpec = describe "balance queries" $ do
  it "returns the caller's balance and any address's balance" $ \env -> do
    setupActors env
    grantNativeTokens env alice 1234
    (_, mine) <- call env alice erc20Trait "get_balance" ""
    mine `shouldBe` returned (amountC 1234)
    (_, theirs) <- call env bob erc20Trait "get_balance_of" (encodeC Cxs addressC alice)
    theirs `shouldBe` returned (amountC 1234)
    (_, none) <- call env bob erc20Trait "get_balance_of" (encodeC Cxs addressC carol)
    none `shouldBe` returned (amountC 0)

  it "reads a missing total supply as zero" $ \env -> do
    setupActors env
    (_, rec) <- call env alice erc20Trait "total_supply" ""
    rec `shouldBe` returned (amountC 0)

  it "faults when invoked at the root (no msg_sender)" $ \env -> do
    tx <- rootTransaction env token (erc20 "get_balance") ""
    run <- runTx env ctx0 tx
    irResult (trRoot run) `shouldBe` Left Deterministic
    trOutput run `shouldBe` InvocationFrameOutput_Fault
    trCommit run `shouldBe` ExecResultTxDeterministicError

deleteSpec :: SpecWith TestEnv
deleteSpec = describe "delete_allowance" $ do
  it "deletes the entry and reports the amount removed" $ \env -> do
    setupActors env
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 300)
    lookupItem env (allowanceKeyOf alice bob) `shouldReturn` Just (WireStorageItem_nnint (X_int128_xdr 0 300))
    (run, rec) <- call env alice erc20Trait "delete_allowance" (encodeC Cxs addressC bob)
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (allowanceDeleteEventBytes alice bob 300)]
    lookupItem env (allowanceKeyOf alice bob) `shouldReturn` Nothing
    allowanceOf env alice bob `shouldReturn` returned (amountC 0)

  it "accepts deleting a missing entry" $ \env -> do
    setupActors env
    (run, rec) <- call env alice erc20Trait "delete_allowance" (encodeC Cxs addressC bob)
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (allowanceDeleteEventBytes alice bob 0)]

  it "regrants from zero after a delete" $ \env -> do
    setupActors env
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 300)
    _ <- call env alice erc20Trait "delete_allowance" (encodeC Cxs addressC bob)
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 5)
    allowanceOf env alice bob `shouldReturn` returned (amountC 5)

  it "always fails delete_balance (native balances cannot be deleted)" $ \env -> do
    setupActors env
    grantNativeTokens env alice 0
    (_, rec) <- call env alice erc20Trait "delete_balance" ""
    rec `shouldBe` faulted

dispatchSpec :: SpecWith TestEnv
dispatchSpec = describe "entrypoint glue" $ do
  it "always fails initialize" $ \env -> do
    setupActors env
    (_, rec) <- call env alice erc20Trait "initialize" (encodeC Cxs addressC alice)
    rec `shouldBe` faulted

  it "aborts with DecodingArgs on malformed calldata, even for initialize" $ \env -> do
    setupActors env
    (_, rec) <- call env alice erc20Trait "transfer" (addrAmount bob 1 <> "\0")
    rec `shouldBe` aborted decodingArgsAbortBytes
    (_, rec2) <- call env alice erc20Trait "initialize" "short"
    rec2 `shouldBe` aborted decodingArgsAbortBytes
    (_, rec3) <- call env alice erc20Trait "get_balance" "\0"
    rec3 `shouldBe` aborted decodingArgsAbortBytes

  it "faults on an unknown method" $ \env -> do
    setupActors env
    (_, rec) <- callWith env ctx0 alice 0x0123456789abcdef ""
    rec `shouldBe` faulted

  it "faults a child that runs out of gas" $ \env -> do
    -- A forwarder that gives the token too little gas to finish a transfer.
    let starved gas = singleMethod $ \ctx -> do
          rec <- invokeRecordGas ctx gas token (erc20 "transfer") (addrAmount bob 1) []
          setReturnBuffer (nscHandler ctx) rec False []
          pure (Right ())
    grantNativeTokens env alice 10
    deployContract env alice (nativeContractXdr kForwarderId)
    let attempt gas = do
          tx <- rootTransaction env alice 0 ""
          run <- runTx env (nativeTestContext [(kForwarderId, starved gas)]) tx
          advanceBlock env
          pure $ case trOutput run of
            InvocationFrameOutput_return_buffer b -> parseRecord b
            _ -> RecMalformed B.empty
    attempt 1 `shouldReturn` faulted
    attempt 200 `shouldReturn` faulted
    nativeBalance env bob `shouldReturn` 0
    attempt 1000000 `shouldReturn` returned ""
    nativeBalance env bob `shouldReturn` 1

permitSpec :: SpecWith TestEnv
permitSpec = describe "ERC20Permit" $ do
  let grant spender amount = encodeC Cxs permitGrantC (PermitGrant spender amount)
      permit = traitMethodId erc20PermitTrait
      runDriver env driver = do
        deployContract env alice (nativeContractXdr kForwarderId)
        tx <- rootTransaction env alice 0 ""
        run <- runTx env (nativeTestContext [(kForwarderId, driver)]) tx
        advanceBlock env
        case trOutput run of
          InvocationFrameOutput_return_buffer b -> pure (run, parseRecord b)
          other -> fail (show other)

  it "spends a grant with transfer_from_signed" $ \env -> do
    grantNativeTokens env alice 1000
    (run, rec) <-
      runDriver env $
        permitDriver token (grant alice 100) (permit "transfer_from_signed") (encodeC Cxs (pairC addressC i128C) (bob, 60)) True
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (transferEventBytes alice bob 60)]
    nativeBalance env alice `shouldReturn` 940
    nativeBalance env bob `shouldReturn` 60

  it "rejects a caller the grant does not name" $ \env -> do
    grantNativeTokens env alice 1000
    (_, rec) <- runDriver env $ permitDriver token (grant bob 100) (permit "transfer_from_signed") (addrAmount bob 60) True
    rec `shouldBe` permitAbort (PermitErr NotSpender)

  it "rejects spending more than the grant" $ \env -> do
    grantNativeTokens env alice 1000
    (_, rec) <- runDriver env $ permitDriver token (grant alice 100) (permit "transfer_from_signed") (addrAmount bob 101) True
    rec `shouldBe` permitAbort (PermitErr ExceedsGrant)

  it "checks the amount's sign after the grant" $ \env -> do
    (_, rec) <- runDriver env $ permitDriver token (grant alice 100) (permit "transfer_from_signed") (addrAmount bob (-1)) True
    rec `shouldBe` permitAbort (PermitErc20Err NegativeAmount)

  it "rejects an over-balance spend with the ERC20 TooLarge" $ \env -> do
    grantNativeTokens env alice 10
    (_, rec) <- runDriver env $ permitDriver token (grant alice 100) (permit "transfer_from_signed") (addrAmount bob 11) True
    rec `shouldBe` permitAbort (PermitErc20Err TooLarge)

  it "turns a grant into allowance with increase_allowance_signed" $ \env -> do
    (run, rec) <- runDriver env $ permitDriver token (grant bob 70) (permit "increase_allowance_signed") "" True
    ok run
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (allowanceChangeEventBytes alice bob 70)]
    setupActors env
    allowanceOf env alice bob `shouldReturn` returned (amountC 70)

  it "deletes an allowance with delete_allowance_signed" $ \env -> do
    setupActors env
    _ <- call env alice erc20Trait "adjust_allowance" (addrAmount bob 30)
    (run, rec) <-
      runDriver env $
        permitDriver token (encodeC Cxs deleteAllowanceGrantC (DeleteAllowanceGrant bob)) (permit "delete_allowance_signed") "" True
    rec `shouldBe` returned ""
    events run `shouldBe` [tokenEvent 3 (allowanceDeleteEventBytes alice bob 30)]
    lookupItem env (allowanceKeyOf alice bob) `shouldReturn` Nothing

  it "aborts with DecodingArgs when no token handle is forwarded" $ \env -> do
    (_, rec) <- runDriver env $ permitDriver token (grant alice 1) (permit "increase_allowance_signed") "" False
    rec `shouldBe` aborted decodingArgsAbortBytes

  it "throws ConsumeDecodeFailure for a grant that does not decode" $ \env -> do
    (_, rec) <- runDriver env $ permitDriver token "garbage" (permit "increase_allowance_signed") "" True
    rec `shouldBe` aborted (libraryExceptionBytes (LibraryExn_auth_token AuthTokenError_kAuthTokenConsumeDecodeFailure))

  it "throws the consume syscall's error for a token minted for someone else" $ \env -> do
    (_, rec) <- runDriver env $ permitDriverFor bob (grant alice 1) (permit "increase_allowance_signed")
    case rec of
      RecFrame st b -> do
        st `shouldBe` Abort
        B.take 10 b `shouldBe` B.take 10 (libraryExceptionBytes (LibraryExn_syscall_exn (SyscallExn 0 0)))
      other -> expectationFailure (show other)
  where
    -- A driver whose token names another consumer: the token contract
    -- cannot consume it.
    permitDriverFor consumer g method = singleMethod $ \ctx -> do
      minted <- mintAuthToken ctx consumer g
      case minted of
        Left _ -> pure (Left Deterministic)
        Right tid -> do
          rec <- invokeRecord ctx token method "" [tid]
          setReturnBuffer (nscHandler ctx) rec False []
          pure (Right ())

erc165Spec :: SpecWith TestEnv
erc165Spec = describe "ERC165" $ do
  it "answers supports_interface for the registered interfaces" $ \env -> do
    setupActors env
    let ask iid = snd <$> call env alice erc165Trait "supports_interface" (encodeC Cxs interfaceIdC iid)
    ask (traitInterfaceId erc20Trait) `shouldReturn` returned "\1"
    ask (traitInterfaceId erc20PermitTrait) `shouldReturn` returned "\1"
    ask (traitInterfaceId erc165Trait) `shouldReturn` returned "\1"
    ask (B.replicate 8 0) `shouldReturn` returned "\0"

  it "answers supports_interfaces in query order" $ \env -> do
    setupActors env
    let ids = [traitInterfaceId erc165Trait, B.replicate 8 9, traitInterfaceId erc20Trait]
    (_, rec) <- call env alice erc165Trait "supports_interfaces" (encodeC Cxs interfaceIdsC ids)
    rec `shouldBe` returned (encodeC Cxs interfaceSupportC [True, False, True])
    (_, tooMany) <- call env alice erc165Trait "supports_interfaces" (encodeC Cxs (vecC 33 interfaceIdC) (replicate 33 (B.replicate 8 0)))
    tooMany `shouldBe` aborted decodingArgsAbortBytes
