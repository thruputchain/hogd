-- | The native default wallet driven through the executor against the
-- in-memory ledger: ports of default_wallet_fallback_test.cc and the
-- native side of default_wallet_parity_test.cc.  The RV32 callees of the
-- C++ tests (calldata_wrapper's echo, manual_revert_test) are replaced by
-- native test contracts with the same observable behavior, and deployable
-- code by mock contracts.
module DefaultWalletSpec (spec) where

import qualified Data.ByteString as B
import qualified Data.Map.Strict as M
import Data.Maybe (fromJust)
import Data.Word (Word64, Word8)
import Harness.Env
import Harness.NativeContracts
import Hogd.BuiltContracts.Codec
import Hogd.BuiltContracts.DefaultWallet
import Hogd.BuiltContracts.ItcErc20 (ERC20Error, Erc20Exn (..), pattern TooLarge)
import Hogd.Crypto.DomainSeparators (makePubkeyAccountPreimage, pubkeyAuthAddress)
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Precompiles.NativeContractBuilder (decodingArgsAbortBytes, syscallException)
import Hogd.Exec.Precompiles.PrecompileDefaultWallet (makeNativeDefaultWalletContract)
import Hogd.Exec.Precompiles.PrecompileNativeToken (transferEventBytes)
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.ContractTraitClient (nscSyscall)
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallHandler (setReturnBuffer, staticGetDeployAddress)
import Hogd.Exec.Runtime.Syscalls (sysBytestringSet)
import Hogd.Exec.Runtime.Types (RuntimeContext)
import Hogd.Genesis.Configuration (makeNativeContract, sha256Xdr)
import Hogd.Genesis.Constants (kNativeTokenContractAddress)
import Hogd.Xdr
import Test.Hspec

kEchoId, kAbortId, kFaultId, kWalletId, kDriverId :: Num a => a
kEchoId = 61
kAbortId = 62
kFaultId = 63
kWalletId = 60
kDriverId = 64

-- | The mock contract deployments use: method 0 writes "init" under key
-- 0x01..; method 1 aborts.
kInitMockId :: Num a => a
kInitMockId = 7

echoAddr, abortAddr, faultAddr, missingAddr :: ContractAddress
echoAddr = contractAddress 220
abortAddr = contractAddress 221
faultAddr = contractAddress 222
missingAddr = contractAddress 240

preimageOf :: Word8 -> PubkeyAccountPreimage
preimageOf seed = makePubkeyAccountPreimage (PublicKey_ed25519 (B.replicate 32 seed)) (fromIntegral seed)

walletOf :: Word8 -> ContractAddress
walletOf = fromJust . pubkeyAuthAddress . preimageOf

wallet :: ContractAddress
wallet = walletOf 1

initKey :: StorageKeyContractData
initKey = B.replicate 32 1

initMock :: NativeContract
initMock =
  mkNativeContract
    [ ( 0
      , \ctx -> do
          (r, ()) <- nscSyscall ctx KBytestringSet () $ \g -> sysBytestringSet (nscHandler ctx) g initKey "init"
          pure (if r == Right KSyscallErrorCodeNone then Right () else Left Deterministic)
      )
    , (1, \ctx -> setReturnBuffer (nscHandler ctx) (exceptionBytes 0x1122334455667788 (7 :: Word64)) True [] >> pure (Right ()))
    ]

walletCtx :: RuntimeContext
walletCtx =
  withMockContext
    (nativeOnlyContext [(kEchoId, echoContract), (kAbortId, abortWithCalldata), (kFaultId, faultContract), (kWalletId, makeNativeDefaultWalletContract)])
    (M.fromList [(kInitMockId, initMock)])

setup :: TestEnv -> IO ()
setup env = do
  deployContract env echoAddr (nativeContractXdr kEchoId)
  deployContract env abortAddr (nativeContractXdr kAbortId)
  deployContract env faultAddr (nativeContractXdr kFaultId)

-- | Root-invoke the (undeployed, so default) wallet.
runWallet :: TestEnv -> B.ByteString -> Calldata -> AuthClaim -> IO TxRun
runWallet env method cd claim = runWalletAt env wallet (traitMethodId defaultWalletTrait method) cd claim

runWalletAt :: TestEnv -> ContractAddress -> MethodID' -> Calldata -> AuthClaim -> IO TxRun
runWalletAt env addr method cd claim = do
  tx <- rootTransactionWithClaim env addr method cd claim
  run <- runTx env walletCtx tx
  advanceBlock env
  pure run

type MethodID' = Word64

execute :: TestEnv -> [WalletCall] -> IO TxRun
execute env calls = runWallet env "execute" (encodeC Cxs walletCallsC calls) (pubkeyClaim (preimageOf 1))

deploy :: TestEnv -> SHA256Hash -> SHA256Hash -> Integer -> Maybe WalletInitCall -> AuthClaim -> IO TxRun
deploy env nonce h amount ini claim = runWallet env "deploy" (encodeC Cxs deployArgsC (nonce, h, amount, ini)) claim

echoCall :: B.ByteString -> WalletCall
echoCall payload = WalletCall echoAddr (B.replicate 8 0) maxBound payload

walletAbort :: WalletError -> InvocationFrameOutput
walletAbort e = InvocationFrameOutput_abort_buffer (serializeContractError (ErrException (WalletExn e)))

-- | An aborted root: the wallet ran (root success) and the body is
-- discarded.
expectReverted :: TxRun -> InvocationFrameOutput -> Expectation
expectReverted run out = do
  irResult (trRoot run) `shouldBe` Right ()
  trCommit run `shouldBe` ExecResultTxRootFrameReverted
  trOutput run `shouldBe` out
  fmap (\l -> l.events) (trEventLog run) `shouldBe` Just []

expectOk :: TxRun -> B.ByteString -> Expectation
expectOk run out = do
  irResult (trRoot run) `shouldBe` Right ()
  trCommit run `shouldBe` ExecResultOK
  trOutput run `shouldBe` InvocationFrameOutput_return_buffer out

syscallAbort :: SyscallNo -> SyscallErrorCode -> InvocationFrameOutput
syscallAbort n c = InvocationFrameOutput_abort_buffer (libraryExceptionBytes (syscallException n c))

spec :: Spec
spec = around withTestEnv $ do
  executeSpec
  authSpec
  calleeSpec
  returnDataSpec
  decodingSpec
  deploySpec

executeSpec :: SpecWith TestEnv
executeSpec = describe "execute (root fallback)" $ do
  it "runs the wallet for an undeployed pubkey address and returns the echoed bytes" $ \env -> do
    setup env
    run <- execute env [echoCall (B.replicate 4 0xAB)]
    -- CXS of Vec<Vec<u8>>{payload}: count, length, bytes.
    expectOk run (B.pack [1, 4, 0xAB, 0xAB, 0xAB, 0xAB])

  it "returns every call's bytes in order" $ \env -> do
    setup env
    run <- execute env [echoCall "a", echoCall "", echoCall "xyz"]
    expectOk run (encodeC Cxs walletReturnsC ["a", "", "xyz"])

  it "reverts an empty call list with EmptyCallList" $ \env -> do
    setup env
    run <- execute env []
    expectReverted run (walletAbort EmptyCallList)

  it "accepts 16 calls and rejects 17 with TooManyCalls" $ \env -> do
    setup env
    ok16 <- execute env (replicate 16 (echoCall "z"))
    expectOk ok16 (encodeC Cxs walletReturnsC (replicate 16 "z"))
    run <- execute env (replicate 17 (echoCall "z"))
    expectReverted run (walletAbort TooManyCalls)

  it "faults an unknown entrypoint" $ \env -> do
    setup env
    run <- runWalletAt env wallet 999 "" (pubkeyClaim (preimageOf 1))
    irResult (trRoot run) `shouldBe` Left Deterministic
    trCommit run `shouldBe` ExecResultTxDeterministicError
    trOutput run `shouldBe` InvocationFrameOutput_Fault

authSpec :: SpecWith TestEnv
authSpec = describe "authorization" $ do
  it "rejects a source-address claim with InvalidAuthClaim" $ \env -> do
    setup env
    run <- runWallet env "execute" (encodeC Cxs walletCallsC [echoCall "a"]) sourceAddressClaim
    expectReverted run (walletAbort InvalidAuthClaim)

  it "rejects a claim for another key with InvalidAuthClaim" $ \env -> do
    setup env
    run <- runWallet env "execute" (encodeC Cxs walletCallsC [echoCall "a"]) (pubkeyClaim (preimageOf 99))
    expectReverted run (walletAbort InvalidAuthClaim)

  it "authorizes a natively deployed wallet the same way" $ \env -> do
    setup env
    let w2 = walletOf 2
    deployContract env w2 (nativeContractXdr kWalletId)
    run <- runWalletAt env w2 (traitMethodId defaultWalletTrait "execute") (encodeC Cxs walletCallsC [echoCall "q"]) (pubkeyClaim (preimageOf 2))
    expectOk run (encodeC Cxs walletReturnsC ["q"])
    bad <- runWalletAt env w2 (traitMethodId defaultWalletTrait "execute") (encodeC Cxs walletCallsC [echoCall "q"]) (pubkeyClaim (preimageOf 1))
    expectReverted bad (walletAbort InvalidAuthClaim)

calleeSpec :: SpecWith TestEnv
calleeSpec = describe "callee outcomes" $ do
  it "forwards a callee's abort verbatim" $ \env -> do
    setup env
    let exn = exceptionBytes 0x82827bef8087f1b6 (1 :: Word64) <> "opaque tail"
    run <- execute env [echoCall "first", WalletCall abortAddr (B.replicate 8 0) maxBound exn]
    expectReverted run (InvocationFrameOutput_abort_buffer exn)

  it "maps an abort without a selector to CallAborted" $ \env -> do
    setup env
    let abortWith b = execute env [WalletCall abortAddr (B.replicate 8 0) maxBound b]
    abortWith "" >>= \run -> expectReverted run (walletAbort CallAborted)
    abortWith "\xff\x01\x02" >>= \run -> expectReverted run (walletAbort CallAborted)
    -- A non-canonical one-byte header is not a selector either.
    abortWith "\x3f\x05" >>= \run -> expectReverted run (walletAbort CallAborted)

  it "maps a faulting callee to CallFaulted" $ \env -> do
    setup env
    run <- execute env [WalletCall faultAddr (B.replicate 8 0) maxBound ""]
    expectReverted run (walletAbort CallFaulted)

  it "maps an out-of-gas callee to CallFaulted" $ \env -> do
    setup env
    -- Native callees run for free, so only a zero budget exhausts one.
    run <- execute env [WalletCall echoAddr (B.replicate 8 0) 0 "x"]
    expectReverted run (walletAbort CallFaulted)

  it "maps a pre-frame rejection (no fallback below the root) to CallFaulted" $ \env -> do
    setup env
    run <- execute env [WalletCall missingAddr (B.replicate 8 0) maxBound "", echoCall "never"]
    expectReverted run (walletAbort CallFaulted)
    run2 <- execute env [WalletCall (walletOf 2) (B.replicate 8 0) maxBound ""]
    expectReverted run2 (walletAbort CallFaulted)

  it "returns a zero-byte child return" $ \env -> do
    setup env
    run <- execute env [echoCall ""]
    expectOk run (B.pack [1, 0])

returnDataSpec :: SpecWith TestEnv
returnDataSpec = describe "return data limit (1024 encoded bytes)" $ do
  -- count (1) + length prefix (3 for 256..65535) + bytes
  it "accepts an exact fit" $ \env -> do
    setup env
    run <- execute env [echoCall (B.replicate 1020 7)]
    expectOk run (encodeC Cxs walletReturnsC [B.replicate 1020 7])

  it "rejects one byte over" $ \env -> do
    setup env
    run <- execute env [echoCall (B.replicate 1021 7)]
    expectReverted run (walletAbort ReturnDataTooLarge)

  it "rejects when a later call overflows the running total" $ \env -> do
    setup env
    ok2 <- execute env [echoCall (B.replicate 500 1), echoCall (B.replicate 500 2)]
    expectOk ok2 (encodeC Cxs walletReturnsC [B.replicate 500 1, B.replicate 500 2])
    run <- execute env [echoCall (B.replicate 500 1), echoCall (B.replicate 500 2), echoCall (B.replicate 20 3)]
    expectReverted run (walletAbort ReturnDataTooLarge)

  it "counts the length prefix width at its breakpoints" $ \env -> do
    setup env
    -- 62 bytes take a 1-byte prefix, 63 a 2-byte one.
    a <- execute env [echoCall (B.replicate 62 0), echoCall (B.replicate 957 0)]
    expectOk a (encodeC Cxs walletReturnsC [B.replicate 62 0, B.replicate 957 0])
    b <- execute env [echoCall (B.replicate 63 0), echoCall (B.replicate 955 0)]
    expectOk b (encodeC Cxs walletReturnsC [B.replicate 63 0, B.replicate 955 0])
    c <- execute env [echoCall (B.replicate 63 0), echoCall (B.replicate 956 0)]
    expectReverted c (walletAbort ReturnDataTooLarge)

decodingSpec :: SpecWith TestEnv
decodingSpec = describe "calldata decoding" $ do
  it "aborts execute with DecodingArgs on malformed calldata" $ \env -> do
    setup env
    let good = encodeC Cxs walletCallsC [echoCall "a"]
    run <- runWallet env "execute" (good <> "\0") (pubkeyClaim (preimageOf 1))
    expectReverted run (InvocationFrameOutput_abort_buffer decodingArgsAbortBytes)
    run2 <- runWallet env "execute" (B.take 5 good) (pubkeyClaim (preimageOf 1))
    expectReverted run2 (InvocationFrameOutput_abort_buffer decodingArgsAbortBytes)

  it "aborts deploy with DecodingArgs on malformed calldata" $ \env -> do
    setup env
    run <- runWallet env "deploy" "\1\2\3" (pubkeyClaim (preimageOf 1))
    expectReverted run (InvocationFrameOutput_abort_buffer decodingArgsAbortBytes)

  it "aborts with DecodingArgs when an auth handle is forwarded" $ \env -> do
    setup env
    let w = walletOf 3
        cd = encodeC Cxs walletCallsC [echoCall "a"]
        driver = singleMethod $ \ctx -> do
          minted <- mintAuthToken ctx w (encodeC Txs u32C 1)
          case minted of
            Left _ -> pure (Left Deterministic)
            Right tid -> do
              rec <- invokeRecord ctx w (traitMethodId defaultWalletTrait "execute") cd [tid]
              setReturnBuffer (nscHandler ctx) rec False []
              pure (Right ())
        rc = withMockContext (nativeOnlyContext [(kEchoId, echoContract), (kWalletId, makeNativeDefaultWalletContract), (kDriverId, driver)]) M.empty
        driverAddr = contractAddress 230
    deployContract env w (nativeContractXdr kWalletId)
    deployContract env driverAddr (nativeContractXdr kDriverId)
    tx <- rootTransaction env driverAddr 0 ""
    run <- runTx env rc tx
    trOutput run `shouldBe` InvocationFrameOutput_return_buffer (B.pack [0, 2] <> decodingArgsAbortBytes)

deploySpec :: SpecWith TestEnv
deploySpec = describe "deploy" $ do
  let code = mockContractXdr kInitMockId
      codeHash = sha256Xdr code
      nonceA = B.replicate 32 9
      target = staticGetDeployAddress wallet nonceA codeHash
      claim = pubkeyClaim (preimageOf 1)

  it "deploys a contract and returns its address" $ \env -> do
    _ <- writeContract env code
    run <- deploy env nonceA codeHash 0 Nothing claim
    expectOk run target
    fmap (\d -> d.hash) <$> lookupDeployment env target `shouldReturn` Just codeHash
    fmap (\l -> l.events) (trEventLog run) `shouldBe` Just []

  it "funds the deployment through the native token" $ \env -> do
    _ <- writeContract env code
    grantNativeTokens env wallet 1000
    run <- deploy env nonceA codeHash 300 Nothing claim
    expectOk run target
    fmap (\l -> l.events) (trEventLog run)
      `shouldBe` Just [TxEmittedEvent {contract_address = kNativeTokenContractAddress, num_topics = 3, serialized_event = transferEventBytes wallet target 300}]
    nativeBalance env wallet `shouldReturn` 700
    nativeBalance env target `shouldReturn` 300

  it "rolls back an underfunded deployment with the token's TooLarge" $ \env -> do
    _ <- writeContract env code
    grantNativeTokens env wallet 10
    run <- deploy env nonceA codeHash 11 Nothing claim
    expectReverted run (InvocationFrameOutput_abort_buffer (serializeContractError (ErrException (Erc20Exn tooLarge))))
    lookupDeployment env target `shouldReturn` Nothing
    nativeBalance env wallet `shouldReturn` 10

  it "rejects unknown code with the kDeployContract syscall exception, rolling back the transfer" $ \env -> do
    grantNativeTokens env wallet 1000
    let missing = B.replicate 32 99
    run <- deploy env nonceA missing 100 Nothing claim
    expectReverted run (syscallAbort KDeployContract KSyscallErrorCodeInvalidSyscallInput)
    nativeBalance env wallet `shouldReturn` 1000
    lookupDeployment env (staticGetDeployAddress wallet nonceA missing) `shouldReturn` Nothing

  it "rejects a duplicate deployment with ContractAlreadyDeployed" $ \env -> do
    _ <- writeContract env code
    first <- deploy env nonceA codeHash 0 Nothing claim
    expectOk first target
    second <- deploy env nonceA codeHash 0 Nothing claim
    expectReverted second (syscallAbort KDeployContract KSyscallErrorCodeContractAlreadyDeployed)

  it "refuses to deploy the precompiles' code" $ \env -> do
    let walletHash = sha256Xdr (makeNativeContract KNativeContractDefaultWallet)
        tokenHash = sha256Xdr (makeNativeContract KNativeContractNativeToken)
    run <- deploy env (B.replicate 32 1) walletHash 0 Nothing claim
    expectReverted run (syscallAbort KDeployContract KSyscallErrorCodeInvalidSyscallInput)
    run2 <- deploy env (B.replicate 32 1) tokenHash 0 Nothing claim
    expectReverted run2 (syscallAbort KDeployContract KSyscallErrorCodeInvalidSyscallInput)

  it "runs the initializer as the wallet and commits its effects" $ \env -> do
    _ <- writeContract env code
    run <- deploy env nonceA codeHash 0 (Just (WalletInitCall (B.replicate 8 0) maxBound "")) claim
    expectOk run target
    lookupItem env (contractDataKey target initKey) `shouldReturn` Just (WireStorageItem_bytestring "init")

  it "rolls everything back when the initializer aborts, forwarding its abort" $ \env -> do
    _ <- writeContract env code
    grantNativeTokens env wallet 1000
    run <- deploy env nonceA codeHash 50 (Just (WalletInitCall (entrypointFromMethodId 1) maxBound "")) claim
    expectReverted run (InvocationFrameOutput_abort_buffer (exceptionBytes 0x1122334455667788 (7 :: Word64)))
    lookupDeployment env target `shouldReturn` Nothing
    nativeBalance env wallet `shouldReturn` 1000

  it "checks the claim before transferring anything" $ \env -> do
    _ <- writeContract env code
    grantNativeTokens env wallet 1000
    run <- deploy env nonceA codeHash 100 Nothing (pubkeyClaim (preimageOf 2))
    expectReverted run (walletAbort InvalidAuthClaim)
    nativeBalance env wallet `shouldReturn` 1000
  where
    tooLarge :: ERC20Error
    tooLarge = TooLarge
