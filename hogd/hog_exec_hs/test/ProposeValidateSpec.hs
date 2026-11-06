-- | Propose → validate round trips (runtime/propose_validate_roundtrip_test.cc):
-- run a transaction in proposal mode, capture its nondeterministic proposal
-- data, then replay the block in validation mode with that data and check
-- it reproduces the result; tampered proposal data must fail validation.
module ProposeValidateSpec (spec) where

import Control.Monad (forM, forM_, unless)
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.Bits (xor)
import Data.IORef
import Data.Word (Word8)
import Harness.Env
import Hogd.Exec.Common.Status
import Hogd.Exec.Precompiles.NativeContractBuilder (hostFnErrorConversion)
import Hogd.Exec.Runtime.ContractTraitClient (nscSyscall, zeroInvokeOutputs)
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallEntry (OutSpan (..), hostOutSpan)
import Hogd.Exec.Runtime.SyscallHandler (abortTransaction, extractProposalData, getCalldata, rootInvoke, tryCommitTransaction)
import Hogd.Exec.Runtime.Syscalls
import Hogd.Exec.Runtime.Types (MethodID, RuntimeContext, SystemCallHandler)
import Hogd.Xdr
import Test.Hspec

-- Native ids of contracts A and B (clear of the precompiles').
kIdA, kIdB :: Num a => a
kIdA = 40
kIdB = 41

addrA, addrB :: ContractAddress
addrA = contractAddress 0xA0
addrB = contractAddress 0xA1

kWriteHello, kWriteWorld :: MethodID
kWriteHello = 0
kWriteWorld = 1

-- | @make_storage_key@: the string, zero-padded to 32 bytes.
storageKey :: String -> StorageKeyContractData
storageKey str = B.take 32 (BC.pack str <> B.replicate 32 0)

setup :: TestEnv -> IO ()
setup env =
  forM_ [(addrA, kIdA), (addrB, kIdB)] $ \(addr, i) -> do
    deployContract env addr (nativeContractXdr i)
    grantNativeTokens env addr 1000000000

abContext :: NativeContract -> NativeContract -> RuntimeContext
abContext a b = nativeTestContext [(kIdA, a), (kIdB, b)]

-- | B: method i writes the i-th (key, value).
writeContract' :: [(String, B.ByteString)] -> NativeContract
writeContract' entries =
  mkNativeContract
    [ (i, \ctx -> bytestringSet ctx (storageKey k) v)
    | (i, (k, v)) <- zip [0 ..] entries
    ]

bytestringSet :: NativeSyscallContext -> StorageKeyContractData -> B.ByteString -> IO (InvokeStatus ())
bytestringSet ctx key value = do
  (r, ()) <- nscSyscall ctx KBytestringSet () $ \g -> sysBytestringSet (nscHandler ctx) g key value
  pure $ case r of
    Right KSyscallErrorCodeNone -> Right ()
    _ -> Left Deterministic

-- | Try-invoke B's @method@ with unbounded gas.
tryInvokeB :: NativeSyscallContext -> MethodID -> IO (HostFnStatus SyscallErrorCode, InvokeOutputs)
tryInvokeB ctx method =
  nscSyscall ctx KInvoke zeroInvokeOutputs $ \g ->
    sysInvoke (nscHandler ctx) g addrB B.empty B.empty (method, maxBound, fromIntegral (let InvokeFlag f = TryInvoke in f))

invokeATx :: TestEnv -> IO Transaction
invokeATx env = rootTransaction env addrA 0 B.empty

-- | Run a transaction as the C++ propose() helper: root invoke (must
-- succeed), extract the proposal data, then commit or abort.
propose :: TestEnv -> RuntimeContext -> Transaction -> Bool -> IO NondeterministicProposalData
propose env rc tx commit = do
  (h, _) <- newHandler env rc tx
  root <- rootInvoke h
  isRootSuccess root `shouldBe` True
  d <- extractProposalData h
  if commit
    then tryCommitTransaction h `shouldReturn` ExecResultOK
    else abortOk h
  pure d

abortOk :: SystemCallHandler -> IO ()
abortOk h = abortTransaction h >>= (`shouldSatisfy` either (const False) (const True))

-- | Validate: root invoke must succeed and the commit go through.
validateOk :: TestEnv -> RuntimeContext -> NondeterministicProposalData -> Transaction -> IO ()
validateOk env rc d tx = do
  (h, _) <- newValidationHandler env rc d tx
  root <- rootInvoke h
  isRootSuccess root `shouldBe` True
  tryCommitTransaction h `shouldReturn` ExecResultOK

-- | Validate with bad data: the root invoke must fail (and the transaction
-- abort cleanly).
validateRejected :: TestEnv -> RuntimeContext -> NondeterministicProposalData -> Transaction -> IO ()
validateRejected env rc d tx = do
  (h, _) <- newValidationHandler env rc d tx
  root <- rootInvoke h
  isRootSuccess root `shouldBe` False
  abortOk h

noopContext :: RuntimeContext
noopContext = nativeTestContext [(kIdA, mkNativeContract [(0, \_ -> pure (Right ()))])]

------------------------------------------------------------------------------
-- Retrocausal queues

rcqKey :: StorageKeyContractData
rcqKey = storageKey "rcq0"

orFail :: HostFnStatus a -> IO (Either InvokeError a)
orFail = either (fmap Left . hostFnErrorConversion) (pure . Right)

rcqInsert :: NativeSyscallContext -> Word8 -> IO (Either InvokeError ())
rcqInsert ctx v = do
  (r, ()) <- nscSyscall ctx KRetrocausalQueueInsert () $ \_ -> sysRetrocausalQueueInsert (nscHandler ctx) rcqKey (B.singleton v)
  fmap (const ()) <$> orFail r

-- | Consume one entry into a buffer of @cap@ bytes; the entry if one was
-- consumed and fit.
rcqConsume :: NativeSyscallContext -> Int -> IO (Either InvokeError (Maybe B.ByteString))
rcqConsume ctx cap = do
  (out, readOut) <- if cap == 0 then pure (OutSpan 0 (const (pure ())), pure B.empty) else hostOutSpan cap
  (r, len) <- nscSyscall ctx KRetrocausalQueueConsume 0 $ \_ -> sysRetrocausalQueueConsume (nscHandler ctx) rcqKey False out
  r' <- orFail r
  case r' of
    Left e -> pure (Left e)
    Right KSyscallErrorCodeNone | cap > 0 && fromIntegral len == cap -> Right . Just <$> readOut
    Right _ -> pure (Right Nothing)

-- | Method 0: insert each calldata byte; method 1: consume calldata[0]
-- entries (default 1).
rcqContext :: RuntimeContext
rcqContext =
  nativeTestContext
    [ ( kIdA
      , mkNativeContract
          [ ( 0
            , \ctx -> do
                cd <- getCalldata (nscHandler ctx)
                loop (map (rcqInsert ctx) (B.unpack cd))
            )
          , ( 1
            , \ctx -> do
                cd <- getCalldata (nscHandler ctx)
                let count = if B.null cd then 1 else fromIntegral (B.head cd)
                loop (replicate count (fmap (const ()) <$> rcqConsume ctx 0))
            )
          ]
      )
    ]
  where
    loop [] = pure (Right ())
    loop (act : rest) = act >>= either (pure . Left) (const (loop rest))

rcqTx :: TestEnv -> MethodID -> [Word8] -> IO Transaction
rcqTx env method cd = rootTransaction env addrA method (B.pack cd)

------------------------------------------------------------------------------

spec :: Spec
spec = do
  it "SuccessfulRoundtrip" $ withTestEnv $ \env -> do
    setup env
    let a = mkNativeContract
          [ ( 0
            , \ctx -> do
                (r0, _) <- tryInvokeB ctx kWriteHello
                (r1, _) <- tryInvokeB ctx kWriteWorld
                case (r0, r1) of
                  (Right _, Right _) -> bytestringSet ctx (storageKey "result") "ok"
                  _ -> pure (Left Deterministic)
            )
          ]
        rc = abContext a (writeContract' [("b_data0", "hello"), ("b_data1", "world")])
    tx <- invokeATx env
    d <- propose env rc tx True
    d.nondeterministic_bit_count `shouldBe` 2
    proposed <- discardBlock env
    validateOk env rc d tx
    validated <- discardBlock env
    validated `shouldBe` proposed

  it "ExtraBitsCauseValidationFailure" $ withTestEnv $ \env -> do
    setup env
    let a = mkNativeContract [(0, \ctx -> either (const (Left Deterministic)) (const (Right ())) . fst <$> tryInvokeB ctx kWriteHello)]
        rc = abContext a (writeContract' [("b_data0", "hello")])
    tx <- invokeATx env
    d <- propose env rc tx False
    d.nondeterministic_bit_count `shouldBe` 1
    validateRejected env rc d {nondeterministic_bit_count = 2} tx

  it "FlippedBitsCauseValidationFailure" $ withTestEnv $ \env -> do
    setup env
    -- The second try-invoke runs only if the first one committed.
    let a = mkNativeContract
          [ ( 0
            , \ctx -> do
                (r0, out0) <- tryInvokeB ctx kWriteHello
                case r0 of
                  Left _ -> pure (Left Deterministic)
                  Right code
                    | code == KSyscallErrorCodeNone && ioFrameStatus out0 == InvocationFrameStatusCode_Return -> do
                        (r1, _) <- tryInvokeB ctx kWriteWorld
                        pure (either (const (Left Deterministic)) (const (Right ())) r1)
                    | otherwise -> pure (Right ())
            )
          ]
        rc = abContext a (writeContract' [("b_data0", "hello"), ("b_data1", "world")])
    tx <- invokeATx env
    d <- propose env rc tx False
    d.nondeterministic_bit_count `shouldBe` 2
    let bits = d.nondeterministic_bits
        tampered = d {nondeterministic_bits = B.cons (B.head bits `xor` 1) (B.tail bits)}
    _ <- discardBlock env
    (h, _) <- newValidationHandler env rc tampered tx
    root <- rootInvoke h
    -- Either the root invoke fails or the commit detects the unconsumed bit.
    if isRootSuccess root
      then tryCommitTransaction h >>= (`shouldNotBe` ExecResultOK)
      else abortOk h

  it "MalformedProposalRejectedEarly" $ withTestEnv $ \env -> do
    setup env
    tx <- invokeATx env
    let malformed = NondeterministicProposalData {nondeterministic_bits = B.empty, nondeterministic_bit_count = 10, observed_queue_entry_sequence = []}
    validateRejected env noopContext malformed tx

  it "PaddingBitEntropyGrinding" $ withTestEnv $ \env -> do
    setup env
    tx <- invokeATx env
    let grinding = NondeterministicProposalData {nondeterministic_bits = B.singleton 0xFF, nondeterministic_bit_count = 1, observed_queue_entry_sequence = []}
    validateRejected env noopContext grinding tx

  it "RCQConcurrentEntryRoundtrip" $ withTestEnv $ \env -> do
    setup env
    txInsert <- rcqTx env 0 [10]
    txConsume <- rcqTx env 1 [1]
    insertP <- propose env rcqContext txInsert True
    consumeP <- propose env rcqContext txConsume True
    consumeP.nondeterministic_bit_count `shouldBe` 1
    consumeP.observed_queue_entry_sequence `shouldBe` [B.singleton 10]
    proposed <- discardBlock env
    validateOk env rcqContext insertP txInsert
    validateOk env rcqContext consumeP txConsume
    validated <- discardBlock env
    -- The validation block reproduces the proposal block's effects.
    validated `shouldBe` proposed

  it "RCQConsumeEmptyRoundtrip" $ withTestEnv $ \env -> do
    setup env
    txInsert <- rcqTx env 0 [1]
    _ <- propose env rcqContext txInsert True
    advanceBlock env
    -- One past entry, then a concurrent miss.
    txConsume <- rcqTx env 1 [2]
    d <- propose env rcqContext txConsume True
    d.nondeterministic_bit_count `shouldBe` 1
    d.observed_queue_entry_sequence `shouldBe` []
    proposed <- discardBlock env
    validateOk env rcqContext d txConsume
    validated <- discardBlock env
    validated `shouldBe` proposed

  it "RCQInsertConsumeSameTxRoundtrip" $ withTestEnv $ \env -> do
    setup env
    consumed <- newIORef []
    let contract = mkNativeContract
          [ ( 0
            , \ctx -> do
                writeIORef consumed []
                ins <- forM [10, 20] (rcqInsert ctx)
                case sequence ins of
                  Left e -> pure (Left e)
                  Right _ -> do
                    outs <- forM [1 :: Int .. 3] (const (rcqConsume ctx 1))
                    case sequence outs of
                      Left e -> pure (Left e)
                      Right es -> do
                        writeIORef consumed (concatMap (maybe [] B.unpack) es)
                        pure (Right ())
            )
          ]
        rc = nativeTestContext [(kIdA, contract)]
    tx <- rcqTx env 0 []
    d <- propose env rc tx True
    d.nondeterministic_bit_count `shouldBe` 1
    readIORef consumed `shouldReturn` [10, 20]
    _ <- discardBlock env
    validateOk env rc d tx
    readIORef consumed `shouldReturn` [10, 20]

  let proposeInsertAndConsume env = do
        txInsert <- rcqTx env 0 [10]
        txConsume <- rcqTx env 1 [1]
        insertP <- propose env rcqContext txInsert True
        consumeP <- propose env rcqContext txConsume False
        pure (txInsert, txConsume, insertP, consumeP)
      tamperedConsumeFails env (txInsert, txConsume, insertP, _) tampered = do
        _ <- discardBlock env
        validateOk env rcqContext insertP txInsert
        validateRejected env rcqContext tampered txConsume

  it "RCQExtraObservationsCauseFailure" $ withTestEnv $ \env -> do
    setup env
    run@(_, _, _, consumeP) <- proposeInsertAndConsume env
    tamperedConsumeFails env run consumeP {observed_queue_entry_sequence = consumeP.observed_queue_entry_sequence ++ [B.singleton 99]}

  it "RCQMissingObservationCausesFailure" $ withTestEnv $ \env -> do
    setup env
    run@(_, _, _, consumeP) <- proposeInsertAndConsume env
    length consumeP.observed_queue_entry_sequence `shouldBe` 1
    unless (null consumeP.observed_queue_entry_sequence) $
      tamperedConsumeFails env run consumeP {observed_queue_entry_sequence = []}
