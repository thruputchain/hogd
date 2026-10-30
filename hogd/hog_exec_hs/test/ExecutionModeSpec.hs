-- | Ports of hogd/hog_exec/components/execution_mode_test.cc.
module ExecutionModeSpec (spec) where

import qualified Data.ByteString as B
import Hogd.Exec.Components.ExecutionMode
import Hogd.Xdr
import Test.Hspec

addr :: Int -> AddressAndKey
addr n =
  AddressAndKey
    { address = B.pack (fromIntegral n : replicate 31 0)
    , key = StorageKey_contract_data_key (B.pack (fromIntegral n : replicate 31 0))
    }

qe :: Int -> QueueEntry
qe n = B.pack [fromIntegral n]

proposal :: IO ProposalModeData
proposal = newExecutionModeData ProposalModeCtor >>= \case
  ModeProposal p -> pure p
  _ -> error "unreachable"

validation :: NondeterministicProposalData -> IO ValidationModeData
validation d = newExecutionModeData (ValidationModeCtor d) >>= \case
  ModeValidation v -> pure v
  _ -> error "unreachable"

emptyData :: NondeterministicProposalData
emptyData = NondeterministicProposalData {nondeterministic_bits = mempty, nondeterministic_bit_count = 0, observed_queue_entry_sequence = []}

spec :: Spec
spec = do
  it "stores proposal bits LSB first across bytes" $ do
    p <- proposal
    let bits = [False, False, False, True, True, False, True, False, False, True]
    mapM_ (logNondeterministicBit p) bits
    d <- proposalData p
    v <- validation d
    validateProposalData v `shouldBe` True
    got <- mapM (const (consumeNondeterministicBit v)) bits
    got `shouldBe` map Just bits
    consumeNondeterministicBit v `shouldReturn` Nothing
    checkAllConsumed v `shouldReturn` True

  it "accepts empty validation data" $ do
    v <- validation emptyData
    validateProposalData v `shouldBe` True
    checkAllConsumed v `shouldReturn` True

  it "rejects malformed bit counts and nonzero padding" $ do
    let bad n = emptyData {nondeterministic_bits = B.pack [0xff], nondeterministic_bit_count = n}
    mapM (\n -> validateProposalData <$> validation (bad n)) [2, 5, 0, 8, 9]
      `shouldReturn` [False, False, False, True, False]

  it "records each queue observation once, in first-observed order" $ do
    p <- proposal
    let obs a i e = logQueueEntryObservation p (addr a) i (qe e)
    mapM_ (\(a, i, e) -> obs a i e) [(1, 0, 1), (1, 0, 1), (1, 0, 1), (1, 0, 1), (1, 1, 2), (1, 2, 3), (2, 0, 4), (1, 1, 2), (1, 1, 2), (1, 0, 1)]
    d <- proposalData p
    d.observed_queue_entry_sequence `shouldBe` map qe [1, 2, 3, 4]
    v <- validation d
    res <- mapM (\(a, i, _) -> consumeQueueEntryObservation v (addr a) i) [(1, 0, 1), (1, 0, 1), (1, 0, 1), (1, 0, 1), (1, 1, 2), (1, 2, 3), (2, 0, 4), (1, 1, 2), (1, 1, 2), (1, 0, 1)]
    res `shouldBe` map (Just . qe) [1, 1, 1, 1, 2, 3, 4, 2, 2, 1]
    checkAllConsumed v `shouldReturn` True

  it "rejects an absent bit after a present observation (#1946)" $ do
    p <- proposal
    logNondeterministicBit p True
    _ <- logQueueEntryObservation p (addr 1) 0 (qe 1)
    logNondeterministicBit p True
    _ <- logQueueEntryObservation p (addr 1) 0 (qe 1)
    d <- proposalData p
    let tampered = d {nondeterministic_bits = B.pack [0x01]}
    v <- validation tampered
    validateProposalData v `shouldBe` True
    consumeQueueEntryPresenceBit v (addr 1) 0 `shouldReturn` Just True
    consumeQueueEntryObservation v (addr 1) 0 `shouldReturn` Just (qe 1)
    consumeQueueEntryPresenceBit v (addr 1) 0 `shouldReturn` Nothing

  it "accepts an absent bit at an unheld index" $ do
    v <- validation emptyData {nondeterministic_bits = B.pack [0x02], nondeterministic_bit_count = 3, observed_queue_entry_sequence = [qe 1]}
    consumeQueueEntryPresenceBit v (addr 1) 0 `shouldReturn` Just False
    consumeQueueEntryPresenceBit v (addr 1) 0 `shouldReturn` Just True
    consumeQueueEntryObservation v (addr 1) 0 `shouldReturn` Just (qe 1)
    consumeQueueEntryPresenceBit v (addr 1) 1 `shouldReturn` Just False
    checkAllConsumed v `shouldReturn` True
