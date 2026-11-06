{-# LANGUAGE ScopedTypeVariables #-}
{-# LANGUAGE TypeApplications #-}

-- | Tests for the parameter, cost and utility tables: ports of
-- network_config_params_test.cc, storage_cost_model_test.cc, the int128
-- parts of xdr_util_test.cc, the gas-price pins of
-- syscall_verify_signature_test.cc, the static_asserts of limits.h /
-- xdr_util.h / types.h / genesis_constants.h, and boundary checks of the
-- cost functions.
module TablesSpec (spec) where

import Control.Exception (AssertionFailed (..), ErrorCall, evaluate, try)
import Data.Bits
import qualified Data.ByteString as B
import Data.Char (intToDigit)
import Data.Int (Int16, Int32, Int64, Int8)
import Data.Word (Word16, Word32, Word64, Word8)
import Hogd.Base.NetworkConfigParams
import Hogd.Base.Types
import Hogd.Base.XdrUtil
import Hogd.Exec.Common.Limits
import Hogd.Exec.Runtime.GasCostTable
import Hogd.Exec.Runtime.SyscallArgParams
import Hogd.Exec.Runtime.SyscallMeta
import Hogd.Exec.Runtime.SyscallTypes
import Hogd.Genesis.Configuration
import Hogd.Genesis.Constants
import Hogd.StorageCost.Model
import Hogd.Xdr
import System.IO.Unsafe (unsafePerformIO)
import Test.Hspec
import Test.QuickCheck hiding ((.&.))
import qualified Test.QuickCheck as QC
import X23.Xdr (xdrDecode, xdrEncodeStrict)

spec :: Spec
spec = do
  networkConfigParamsSpec
  limitsSpec
  gasCostSpec
  storageCostSpec
  int128Spec
  genesisSpec
  syscallArgSpec
  syscallMetaSpec

------------------------------------------------------------------------------
-- network_config_params_test.cc

kNames :: [(ConfigParamID, String)]
kNames =
  [ (KConfigParamMaxSHA256Buffers, "max_sha256_buffers")
  , (KConfigParamMaxCrossInvokeDepth, "max_cross_invoke_depth")
  , (KConfigParamMaxNewContractsPerTx, "max_new_contracts_per_tx")
  , (KConfigParamMaxBytestringLen, "max_bytestring_len")
  , (KConfigParamMaxContractSizeBytes, "max_contract_size_bytes")
  , (KConfigParamMaxSavepointDepth, "max_savepoint_depth")
  , (KConfigParamMaxSHA256BufferBytes, "max_sha256_buffer_bytes")
  , (KConfigParamMaxAuthTokenDepth, "max_auth_token_depth")
  , (KConfigParamMaxRv32CodeBytes, "max_rv32_code_bytes")
  , (KConfigParamMaxRv32MemoryBytes, "max_rv32_memory_bytes")
  , (KConfigParamMaxAuthTokens, "max_auth_tokens")
  , (KConfigParamMaxCalldataLen, "max_calldata_len")
  , (KConfigParamMaxReturnBufferLen, "max_return_buffer_len")
  , (KConfigParamMaxTxGas, "max_tx_gas")
  ]

emptyConfig :: NetworkConfig
emptyConfig = NetworkConfig_v0 (NetworkConfigV0 [])

v0Params :: NetworkConfig -> [ConfigParam]
v0Params (NetworkConfig_v0 v0) = v0.params

throwsError :: a -> Expectation
throwsError x = evaluate x `shouldThrow` (\(_ :: ErrorCall) -> True)

networkConfigParamsSpec :: Spec
networkConfigParamsSpec = describe "NetworkConfigParams" $ do
  it "every parameter carries its wire name" $
    mapM_ (\(pid, n) -> do configParamName pid `shouldBe` n; configParamId n `shouldBe` Just pid) kNames
  it "the highest id is the last one named" $ do
    let ConfigParamID h = kHighestConfigParamID
    h `shouldBe` fromIntegral (length kNames) - 1
    configParamName kHighestConfigParamID `shouldBe` snd (last kNames)
    allConfigParamIDs `shouldBe` map fst kNames
  it "an unnamed parameter is not found" $ do
    configParamId "" `shouldBe` Nothing
    configParamId "max_rv32_memmory_bytes" `shouldBe` Nothing
    configParamId "kConfigParamMaxBytestringLen" `shouldBe` Nothing
  it "reads the arm the parameter names" $
    mapM_
      ( \(value, (pid, _)) -> do
          let param = makeParam pid value
          discriminant_ConfigParam param `shouldBe` pid
          configParamValue param `shouldBe` value
      )
      (zip [1001 ..] kNames)
  it "the tx gas arm carries a full GasAmount" $ do
    let kValue = (1 `shiftL` 40) + 7 :: Word64
    configParamMaxValue KConfigParamMaxTxGas `shouldBe` maxBound
    configParamMaxValue KConfigParamMaxReturnBufferLen `shouldBe` fromIntegral (maxBound :: Word32)
    let param = makeParam KConfigParamMaxTxGas kValue
    param `shouldBe` ConfigParam_max_tx_gas kValue
    configParamValue param `shouldBe` kValue
    (networkConfigParamsOf (setParam emptyConfig KConfigParamMaxTxGas kValue)).max_tx_gas `shouldBe` kValue
  it "a uint32 arm refuses a wider value" $ do
    throwsError (makeParam KConfigParamMaxCalldataLen (fromIntegral (maxBound :: Word32) + 1))
    throwsError (setParam emptyConfig KConfigParamMaxSHA256Buffers (fromIntegral (maxBound :: Word32) + 1))
    makeParam KConfigParamMaxCalldataLen (fromIntegral (maxBound :: Word32))
      `shouldBe` ConfigParam_max_calldata_len maxBound
  it "a list omitting tx gas has the default budget" $ do
    let config = setParam emptyConfig KConfigParamMaxReturnBufferLen (fromIntegral kDefaultMaxReturnBufferLen)
    length (v0Params config) `shouldBe` 13
    (networkConfigParamsOf config).max_tx_gas `shouldBe` 1000000000
  it "stated defaults states every parameter at its default" $ do
    let NetworkConfig_v0 v0 = statedDefaults
    isCanonical v0 `shouldBe` True
    length v0.params `shouldBe` length kNames
    networkConfigParams v0 `shouldBe` defaultNetworkConfigParams
  -- Beyond the C++ tests.
  it "has the documented defaults" $ do
    let p = defaultNetworkConfigParams
    map (inForce p . fst) kNames
      `shouldBe` [32, 16, 4, 1024, 524288, 1024, 4096, 4, 262144, 65536, 64, 65536, 1024, 1000000000]
  it "an empty list resolves to the defaults" $
    networkConfigParamsOf emptyConfig `shouldBe` defaultNetworkConfigParams
  it "set_param keeps every other value in force" $ do
    let c1 = setParam statedDefaults KConfigParamMaxCrossInvokeDepth 3
        c2 = setParam c1 KConfigParamMaxSHA256Buffers 7
        p = networkConfigParamsOf c2
    p.max_cross_invoke_depth `shouldBe` 3
    p.max_sha256_buffers `shouldBe` 7
    p {max_cross_invoke_depth = 16, max_sha256_buffers = 32} `shouldBe` defaultNetworkConfigParams
    length (v0Params c2) `shouldBe` 14
  it "set_param on a short list states only the prefix up to the id" $ do
    let c = setParam emptyConfig KConfigParamMaxBytestringLen 99
    map discriminant_ConfigParam (v0Params c) `shouldBe` take 4 (map fst kNames)
    (networkConfigParamsOf c).max_bytestring_len `shouldBe` 99
  it "to_network_config is canonical and reads back" $ do
    let p = defaultNetworkConfigParams {max_tx_gas = 5, max_auth_tokens = 9}
        NetworkConfig_v0 v0 = toNetworkConfig p
    isCanonical v0 `shouldBe` True
    networkConfigParams v0 `shouldBe` p
    toNetworkConfig defaultNetworkConfigParams `shouldBe` statedDefaults
  it "is_canonical rejects gaps and reordering" $ do
    isCanonical (NetworkConfigV0 []) `shouldBe` True
    isCanonical (NetworkConfigV0 [ConfigParam_max_cross_invoke_depth 1]) `shouldBe` False
    isCanonical (NetworkConfigV0 [ConfigParam_max_sha256_buffers 1, ConfigParam_max_sha256_buffers 2]) `shouldBe` False
    configHashable (NetworkConfig_v0 (NetworkConfigV0 [ConfigParam_max_sha256_buffers 1])) `shouldBe` True
  it "a non-canonical list is a CHECK failure, also through set_param" $ do
    let bad = NetworkConfigV0 [ConfigParam_max_cross_invoke_depth 1]
    throwsError (networkConfigParams bad)
    throwsError (setParam (NetworkConfig_v0 bad) KConfigParamMaxSHA256Buffers 1)
  it "only the ids the constructor reads are checked" $ do
    -- 15 entries: index 14 is past kHighestConfigParamID, so stated_param
    -- never looks at it, though is_canonical rejects the list.
    let NetworkConfig_v0 full = statedDefaults
        longer = NetworkConfigV0 (full.params ++ [ConfigParam_max_sha256_buffers 1])
    isCanonical longer `shouldBe` False
    networkConfigParams longer `shouldBe` defaultNetworkConfigParams
  it "the stated defaults survive an XDR round trip" $
    xdrDecode (xdrEncodeStrict statedDefaults) `shouldBe` Right statedDefaults

------------------------------------------------------------------------------
-- limits.h

limitsSpec :: Spec
limitsSpec = describe "Limits" $ do
  it "holds the limits.h static_asserts" $ do
    kMaxBytesTransaction < kMaxBytesSignedTransaction `shouldBe` True
    kMaxSerializedContractBytes + kiB 1 <= kExecToDbMessageCap `shouldBe` True
    kRv32DefaultMaxMemoryBytes <= kRv32MemoryCeilingBytes `shouldBe` True
    kRv32DefaultMaxCodeBytes <= kRv32CodeCeilingBytes `shouldBe` True
    kRv32DefaultMaxMemoryBytes `shouldBe` kDefaultMaxRv32MemoryBytes
    kRv32DefaultMaxCodeBytes `shouldBe` kDefaultMaxRv32CodeBytes
  it "has the C++ values" $ do
    kMaxBytesTransaction `shouldBe` 1900000
    kSavepointDepthCeiling `shouldBe` 1024
    kExecToDbMessageCap `shouldBe` 16777216
    kContractSizeCeilingBytes `shouldBe` 1900000
    kMaxSerializedContractBytes `shouldBe` 1900016
    serializedContractBound 0 `shouldBe` 16
  it "the defaults pass genesis load" $ do
    checkGenesisNetworkConfig statedDefaults `shouldBe` Right defaultNetworkConfigParams
    checkGenesisNetworkConfig emptyConfig `shouldBe` Right defaultNetworkConfigParams
    checkRecoveredTipConfigParams defaultNetworkConfigParams `shouldBe` Right ()
  it "rejects a non-canonical genesis before reading it" $
    checkGenesisNetworkConfig (NetworkConfig_v0 (NetworkConfigV0 [ConfigParam_max_tx_gas 1]))
      `shouldSatisfy` isLeft
  let d = defaultNetworkConfigParams
      ok p = checkGenesisConfigParams p `shouldBe` Right ()
      bad p = checkGenesisConfigParams p `shouldSatisfy` isLeft
  it "caps max_contract_size_bytes at the transaction size" $ do
    ok d {max_contract_size_bytes = 1900000, max_new_contracts_per_tx = 1}
    bad d {max_contract_size_bytes = 1900001, max_new_contracts_per_tx = 1}
  it "caps the commit batch at one message" $ do
    -- min(count * (size + 16), 1900000 + count * 16) + 1024 <= 16 MiB
    -- carried batch: 1900000 + 16 c + 1024 <= 16777216  <=>  c <= 929762
    ok d {max_contract_size_bytes = 1900000, max_new_contracts_per_tx = 929762}
    bad d {max_contract_size_bytes = 1900000, max_new_contracts_per_tx = 929763}
    -- every row pays its envelope, whichever bound is smaller:
    -- 16 c + 1024 <= 16 MiB  <=>  c <= 1048512
    ok d {max_contract_size_bytes = 0, max_new_contracts_per_tx = 1048512}
    bad d {max_contract_size_bytes = 0, max_new_contracts_per_tx = 1048513}
    bad d {max_contract_size_bytes = 0, max_new_contracts_per_tx = maxBound}
    checkGenesisConfigParams d {max_contract_size_bytes = 1900000, max_new_contracts_per_tx = 929763}
      `shouldBe` Left
        "genesis max_new_contracts_per_tx 929763 of max_contract_size_bytes 1900000 needs a \
        \16777232-byte commit batch; this build sends at most 16777216 in one message"
  it "caps the RV32 code and memory bounds" $ do
    ok d {max_rv32_code_bytes = 4194304, max_rv32_memory_bytes = 16777216}
    bad d {max_rv32_code_bytes = 4194305}
    bad d {max_rv32_memory_bytes = 16777217}
  it "bounds max_savepoint_depth to [1, ceiling]" $ do
    ok d {max_savepoint_depth = 1}
    ok d {max_savepoint_depth = 1024}
    bad d {max_savepoint_depth = 0}
    bad d {max_savepoint_depth = 1025}
    checkRecoveredTipConfigParams d {max_savepoint_depth = 0} `shouldSatisfy` isLeft
    checkRecoveredTipConfigParams d {max_savepoint_depth = 1025} `shouldSatisfy` isLeft
    checkRecoveredTipConfigParams d {max_savepoint_depth = 1024} `shouldBe` Right ()
  it "caps max_bytestring_len at the wire ceiling" $ do
    ok d {max_bytestring_len = 65000}
    checkGenesisConfigParams d {max_bytestring_len = 65001}
      `shouldBe` Left "genesis max_bytestring_len 65001 exceeds the protocol ceiling of 65000"

isLeft :: Either a b -> Bool
isLeft = either (const True) (const False)

------------------------------------------------------------------------------
-- gas_cost_table.h

-- | Whether 'Control.Exception.assert' is live in this build (it is
-- compiled out under -O, as C++ asserts are under -DNDEBUG).
assertsEnabled :: Bool
assertsEnabled = unsafePerformIO $ do
  r <- try (evaluate (sha256Blocks maxBound))
  pure $ case r of
    Left (AssertionFailed _) -> True
    Right _ -> False
{-# NOINLINE assertsEnabled #-}

gasCostSpec :: Spec
gasCostSpec = describe "GasCostTable" $ do
  it "has the calibrated constants" $ do
    kSyscallBaseCost `shouldBe` 50
    kSHA256HashBlockCost `shouldBe` 22
    kSHA256StreamCost `shouldBe` 26
    kVerifySignatureBaseCost `shouldBe` 17750
    kVerifySignatureBlockCost `shouldBe` 131
    kVerifySignatureMlDsa65BaseCost `shouldBe` 46592
    kVerifySignatureMlDsa65BlockCost `shouldBe` 160
    kStorageAccessCost `shouldBe` 150
    kDBRPCCost `shouldBe` 37300
    kBytestringBlockBytes `shouldBe` 512
    kBytestringWriteBlockCost `shouldBe` 13
    kBytestringReadBlockCost `shouldBe` 7
    kNoSyscallCost `shouldBe` 10
    kGenericBadSyscallCost `shouldBe` 10
    kMaxCostBytes `shouldBe` 4294967296
  it "hashes the schedule in digest order" $ do
    map (\e -> (e.name, e.cost)) kSyscallGasCostSchedule
      `shouldBe` [ ("SyscallBase", 50)
                 , ("SHA256HashBlock", 22)
                 , ("SHA256Stream", 26)
                 , ("VerifySignatureBase", 17750)
                 , ("VerifySignatureBlock", 131)
                 , ("StorageAccess", 150)
                 , ("DBRPC", 37300)
                 , ("BytestringBlockBytes", 512)
                 , ("BytestringWriteBlock", 13)
                 , ("BytestringReadBlock", 7)
                 , ("NoSyscall", 10)
                 , ("GenericBadSyscall", 10)
                 , ("VerifySignatureMlDsa65Base", 46592)
                 , ("VerifySignatureMlDsa65Block", 160)
                 ]
  it "counts SHA-256 blocks with padding" $ do
    map sha256Blocks [0, 1, 55, 56, 64, 119, 120, 4096] `shouldBe` [1, 1, 1, 2, 2, 2, 3, 65]
    sha256Blocks kMaxCostBytes `shouldBe` 67108865
    map sha256Hash [0, 55, 56] `shouldBe` [22, 22, 44]
  it "counts SHA-512 and SHAKE256 blocks with padding" $ do
    map sha512Blocks [0, 111, 112, 239, 240] `shouldBe` [1, 1, 2, 2, 3]
    map shake256Blocks [0, 135, 136, 271, 272] `shouldBe` [1, 1, 2, 2, 3]
  it "charges sha256_update for whole blocks only" $ do
    map sha256Update [0, 63, 64, 127, 128] `shouldBe` [26, 26, 48, 48, 70]
    sha256Update kMaxCostBytes `shouldBe` 26 + 22 * 67108864
  it "rounds bytestring blocks up, an empty bytestring costing none" $ do
    map bytestringBlocks [0, 1, 512, 513, 1024] `shouldBe` [0, 1, 1, 2, 2]
    map bytestringSet [0, 1, 1024, 65000] `shouldBe` [150, 163, 176, 150 + 13 * 127]
    map bytestringGet [0, 1, 1024, 65000] `shouldBe` [150, 157, 164, 150 + 7 * 127]
  it "prices ed25519 over R || A || message" $ do
    -- 64 + len + 17 padding: a second SHA-512 block from len 48
    map verifySignature [0, 47, 48, 100] `shouldBe` [17881, 17881, 18012, 18012]
  it "covers the calibrated ML-DSA-65 measurement" $
    -- syscall_verify_signature_test.cc: CoversTheCalibratedMeasurement
    mapM_
      ( \(len, billed, measured) -> do
          let b = kSyscallBaseCost + verifySignatureMlDsa65 len
          b `shouldBe` billed
          b `shouldSatisfy` (>= measured)
      )
      [(0, 46802, 46802), (64, 46802, 46734), (1024, 48082, 48071 :: Word64)]
  it "wraps like a release build past kMaxCostBytes" $
    if assertsEnabled
      then pendingWith "asserts are enabled in this build (C++ debug semantics)"
      else do
        -- (2^64 - 1 + 9 + 63) mod 2^64 = 71
        sha256Blocks maxBound `shouldBe` 1
        sha256Hash maxBound `shouldBe` 22
        -- (2^64 - 1 + 511) mod 2^64 = 510
        bytestringBlocks maxBound `shouldBe` 0
        bytestringSet maxBound `shouldBe` 150
        -- maxBound + 64 wraps to 63
        verifySignature maxBound `shouldBe` 17881
        -- maxBound + 66 wraps to 65: (65 + 1 + 135) / 136 = 1
        verifySignatureMlDsa65 maxBound `shouldBe` 46752
        sha256Update maxBound `shouldBe` 26 + 22 * (2 ^ (58 :: Int) - 1)
  it "saturates gas accumulation" $ do
    saturatingAddGas 1 2 `shouldBe` 3
    saturatingAddGas maxBound 0 `shouldBe` maxBound
    saturatingAddGas (maxBound - 49) kSyscallBaseCost `shouldBe` maxBound
    saturatingAddGas (maxBound - 50) kSyscallBaseCost `shouldBe` maxBound
    saturatingAddGas (maxBound - 51) kSyscallBaseCost `shouldBe` maxBound - 1
  it "wraps gas amounts" $
    (WrappedGasAmount 5).get `shouldBe` 5

------------------------------------------------------------------------------
-- storage_cost_model_test.cc

storageCostSpec :: Spec
storageCostSpec = describe "StorageCostModel" $ do
  it "round-trips the default cost model" $ do
    let xdr = storageCostModelToXdr defaultCostModel
    storageCostModelToXdr (storageCostModelFromXdr xdr) `shouldBe` xdr
  it "round-trips custom values" $ do
    let original =
          StorageCostModelXdr
            { ext = ExtensionPoint_0
            , base_data_item_cost = 42
            , bytestring_cost_per_byte = 7
            , base_contract_deployment_cost = 100
            , contract_deployment_cost_per_byte = 3
            , auth_per_key_cost = 55
            , rcq_cost_per_entry = 11
            , replay_cache_cost_per_entry = 17
            , balance_cost = 9999
            }
    storageCostModelToXdr (storageCostModelFromXdr original) `shouldBe` original
  it "get_bytestring_cost does not truncate at 64 bits" $ do
    let largeLen = 2000000000000000000 :: Word64
    toInteger (getBytestringCost defaultCostModel largeLen)
      `shouldBe` 12000 + toInteger largeLen * 10
  it "contract deployment cost includes the base" $ do
    let model =
          storageCostModelFromXdr
            StorageCostModelXdr
              { ext = ExtensionPoint_0
              , base_data_item_cost = 5000
              , bytestring_cost_per_byte = 10
              , base_contract_deployment_cost = 12000
              , contract_deployment_cost_per_byte = 10
              , auth_per_key_cost = 1111
              , rcq_cost_per_entry = 8
              , replay_cache_cost_per_entry = 7
              , balance_cost = 1234
              }
        h size ty = DeployedContractHash {hash = B.replicate 32 0, contract_size = size, contract_type = ty}
    getDeploymentCost model (h 100 KRv32) `shouldBe` 13000
    getDeploymentCost model (h 0 KRv32) `shouldBe` 12000
    getDeploymentCost model (h 0 KNative) `shouldBe` 0
    getDeploymentCost model (h 100 KMockTest) `shouldBe` 0
    throwsError (getDeploymentCost model (h 0 (ContractType 9)))
  -- Beyond the C++ tests.
  it "prices the other items" $ do
    let m = defaultCostModel
    getNnintCost m `shouldBe` 12000
    getBalanceCost m `shouldBe` 1234
    getAuthDataCost m 3 `shouldBe` 12000 + 3 * 1111
    getReplayCacheCost m 5 `shouldBe` 12000 + 5 * 7
    getRetrocausalQueueCost m 0 `shouldBe` 0
    getRetrocausalQueueCost m 2 `shouldBe` 12000 + 16
    -- the size is signed and priced as written
    getRetrocausalQueueCost m (-1) `shouldBe` 12000 - 8
    getBytestringCost m 0 `shouldBe` 12000
  it "the raw constructor fills deployment and auth defaults" $ do
    let m = storageCostModelFromRaw 5 6 7 8 9
    m `shouldBe` StorageCostModel 5 6 5 1 1 7 8 9
  it "wraps int128 products like the C++" $ do
    let m = storageCostModelFromRaw maxBound 0 0 maxBound 0
        exact = toInteger (maxBound :: Word64) + toInteger (maxBound :: Word64) * toInteger (maxBound :: Word64)
    -- (2^64-1)^2 + 2^64 - 1 < 2^128 but > 2^127: wraps negative
    toInteger (getReplayCacheCost m maxBound) `shouldBe` exact - 2 ^ (128 :: Int)

------------------------------------------------------------------------------
-- types.h / xdr_util.h

genInt128 :: Gen Int128
genInt128 =
  oneof
    [ int128FromLimbs <$> arbitrary <*> arbitrary
    , fromInteger <$> choose (-1000, 1000)
    , elements [minBound, maxBound, 0, -1, 1, bit 64, bit 63, -(bit 64)]
    ]

wrap128 :: Integer -> Integer
wrap128 n = let m = n `mod` (2 ^ (128 :: Int)) in if m >= 2 ^ (127 :: Int) then m - 2 ^ (128 :: Int) else m

hex :: B.ByteString -> String
hex = concatMap (\b -> [intToDigit (fromIntegral (b `shiftR` 4)), intToDigit (fromIntegral (b .&. 15))]) . B.unpack

int128Spec :: Spec
int128Spec = describe "Int128" $ do
  it "holds the xdr_util.h static_asserts" $ do
    ((-1 :: Int128) `shiftR` 64) `shouldBe` -1
    int128ToXdr (-1) `shouldBe` X_int128_xdr {highbits = -1, lowbits = maxBound}
    int128FromXdr (int128ToXdr 0) `shouldBe` 0
    int128FromXdr (int128ToXdr (-1)) `shouldBe` -1
    int128FromXdr (int128ToXdr 1) `shouldBe` 1
    int128FromXdr (int128ToXdr (bit 127)) `shouldBe` bit 127
    (bit 127 :: Int128) `shouldBe` minBound
    let i128max = (fromIntegral (maxBound :: Int64) `shiftL` 64) .|. fromIntegral (maxBound :: Word64) :: Int128
    i128max `shouldBe` maxBound
    int128FromXdr (int128ToXdr i128max) `shouldBe` i128max
    word128FromLimbs maxBound maxBound `shouldBe` int128ToWord128 (-1)
    ((fromIntegral (-1 :: Int64) `shiftL` 64) .|. fromIntegral (maxBound :: Word64) :: Int128) `shouldBe` -1
  it "holds the types.h layout static_assert" $ do
    int128ToLE 1 `shouldBe` B.pack (1 : replicate 15 0)
    int128ToLE (-1) `shouldBe` B.replicate 16 0xff
    int128ToLE (bit 64) `shouldBe` B.pack (replicate 8 0 ++ [1] ++ replicate 7 0)
  it "round-trips the little-endian layout" $
    property $ forAll genInt128 $ \v -> int128FromLE (int128ToLE v) === Just v
  it "int128_to_xdr_bytes matches XDR serialization exactly" $
    property $ forAll genInt128 $ \v ->
      int128ToXdrBytes v === xdrEncodeStrict (int128ToXdr v)
  it "round-trips positive and negative values through the row bytes" $ do
    let kPositive = int128FromLimbs 0x0102030405060708 0x090a0b0c0d0e0f10
        kNegative = -kPositive
    int128FromXdrBytes (int128ToXdrBytes kPositive) `shouldBe` Just kPositive
    int128FromXdrBytes (int128ToXdrBytes kNegative) `shouldBe` Just kNegative
    hex (int128ToXdrBytes kPositive) `shouldBe` "0102030405060708090a0b0c0d0e0f10"
    int128FromXdrBytes (B.replicate 15 0) `shouldBe` Nothing
  it "round-trips through int128_xdr" $
    property $ forAll genInt128 $ \v -> int128FromXdr (int128ToXdr v) === v
  it "round-trips uint128 through uint128_xdr" $
    property $ \h l -> word128ToXdr (word128FromXdr (X_uint128_xdr h l)) === X_uint128_xdr h l
  it "wraps arithmetic modulo 2^128" $
    property $ forAll genInt128 $ \a -> forAll genInt128 $ \b ->
      toInteger (a + b) === wrap128 (toInteger a + toInteger b)
        QC..&&. toInteger (a - b) === wrap128 (toInteger a - toInteger b)
        QC..&&. toInteger (a * b) === wrap128 (toInteger a * toInteger b)
  it "detects overflow like __builtin_add_overflow" $ do
    integerAddOverflow @Int128 (maxBound :: Int128) (1 :: Int128) `shouldBe` (True, minBound)
    integerAddOverflow @Int128 (maxBound :: Int128) (-1 :: Int64) `shouldBe` (False, maxBound - 1)
    integerSubOverflow @Int128 (minBound :: Int128) (1 :: Int128) `shouldBe` (True, maxBound)
    integerMulOverflow @Int128 (bit 64 :: Int128) (bit 63 :: Int128) `shouldBe` (True, minBound)
    integerMulOverflow @Int128 (bit 63 :: Int128) (bit 63 :: Int128) `shouldBe` (False, bit 126)
    -- mixed operand and result types, as the builtin allows
    integerAddOverflow @Word64 (5 :: Word64) (-6 :: Int64) `shouldBe` (True, maxBound)
    integerAddOverflow @Word32 (maxBound :: Word32) (1 :: Word64) `shouldBe` (True, 0)
    integerAddOverflows @Int128 (maxBound :: Int128) (0 :: Int) `shouldBe` False
    overflowingAdd (maxBound :: Word64) 1 `shouldBe` (0, True)
    overflowingSub (0 :: Word64) 1 `shouldBe` (maxBound, True)
    overflowingMul (bit 32 :: Word64) (bit 32) `shouldBe` (0, True)
    checkedAdd (maxBound :: Int128) 0 `shouldBe` Just maxBound
    checkedAdd (maxBound :: Int128) 1 `shouldBe` Nothing
    checkedSub (minBound :: Int64) 1 `shouldBe` Nothing
    checkedMul (3 :: Int128) 4 `shouldBe` Just 12
    wrappingAdd (maxBound :: Int128) 1 `shouldBe` minBound
    wrappingSub (minBound :: Int128) 1 `shouldBe` maxBound
  it "converts between widths modularly" $ do
    wrapIntegral @Int64 (bit 64 + 5 :: Int128) `shouldBe` 5
    wrapIntegral @Word128 (-1 :: Int128) `shouldBe` maxBound
    word128ToInt128 maxBound `shouldBe` -1
    checkedIntegral @Int64 (bit 63 :: Int128) `shouldBe` Nothing
    checkedIntegral @Int64 (bit 62 :: Int128) `shouldBe` Just (bit 62)
    fitsIn @Word64 (-1 :: Int128) `shouldBe` False
  it "divides like C++ (truncating)" $ do
    ((-7) :: Int128) `quot` 2 `shouldBe` -3
    ((-7) :: Int128) `rem` 2 `shouldBe` -1
    evaluate ((1 :: Int128) `quot` 0) `shouldThrow` anyArithException
    evaluate ((minBound :: Int128) `quot` (-1)) `shouldThrow` anyArithException
  it "has 128-bit bit operations" $ do
    popCount (-1 :: Int128) `shouldBe` 128
    countLeadingZeros (1 :: Word128) `shouldBe` 127
    countTrailingZeros (bit 100 :: Word128) `shouldBe` 100
    complement (0 :: Word128) `shouldBe` maxBound
    rotate (bit 127 :: Word128) 1 `shouldBe` 1
    (1 :: Int128) `shiftL` 127 `shouldBe` minBound
    (1 :: Int128) `shiftL` 128 `shouldBe` 0
  it "has the size literals" $ do
    kiB 1 `shouldBe` 1024
    miB 1 `shouldBe` 1048576
    giB 2 `shouldBe` 2147483648
    kiB (bit 60) `shouldBe` 0
  it "formats byte arrays" $ do
    showByteArray (B.pack [1, 2, 255]) `shouldBe` "[1, 2, 255]"
    encodeHex (B.pack [0, 171, 255]) `shouldBe` "00abff"
    kNoBatchSizeLimit `shouldBe` maxBound
  it "carries nondeterministic values" $
    getNondeterministic (Nondeterministic True) `shouldBe` True

------------------------------------------------------------------------------
-- genesis_constants.h / genesis_configuration.h

genesisSpec :: Spec
genesisSpec = describe "Genesis" $ do
  it "puts the genesis addresses in the last four bytes" $ do
    kNullContractAddress `shouldBe` B.replicate 32 0
    kNativeTokenContractAddress `shouldBe` B.replicate 31 0 <> B.pack [1]
    kGenesisRootContractAddress `shouldBe` B.replicate 31 0 <> B.pack [2]
    genesisContractAddress (GenesisContractAddresses 0x01020304) `shouldBe` B.replicate 28 0 <> B.pack [1, 2, 3, 4]
    genesisContractAddress (GenesisContractAddresses (-1)) `shouldBe` B.replicate 28 0 <> B.replicate 4 0xff
    makePrecompileAddress KAddressGenesisRoot `shouldBe` kGenesisRootContractAddress
    kNativeTokenTotalSupplyKey `shouldBe` B.replicate 32 0
  it "hashes the native default wallet" $ do
    let h = nativeDefaultWalletHash
    hex h.hash `shouldBe` "0f585dd518ed0644f3edfd3f7d5012cc9f445c4c9d24e168e694c4d6f36faea6"
    h.contract_size `shouldBe` 0
    h.contract_type `shouldBe` KNative
  it "sizes an RV32 deployment by its blob" $ do
    let c = Contract_rv32_contract (ContractBlob (B.replicate 10 7))
    (makeDeployedContractHash c).contract_size `shouldBe` 10
    (makeDeployedContractHash c).contract_type `shouldBe` KRv32
  it "derives genesis deployment addresses from the root" $
    hex (calculateGenesisDeploymentAddress (B.replicate 32 9) (B.replicate 32 7))
      `shouldBe` "d75eaecdf4c1fb2107407bb5a2f641d3b0a04f83a67ca9d634eb4e95818c26cf"
  it "runs the stock chain at the stated defaults" $
    stockNetworkConfig `shouldBe` statedDefaults
  it "visits the execution prerequisites in order" $ do
    let root = Contract_rv32_contract (ContractBlob "root")
        config =
          GenesisConfiguration
            { network_hash = B.replicate 32 0
            , storage_cost_model = storageCostModelToXdr defaultCostModel
            , network_config = statedDefaults
            , root_contract = root
            , default_wallet = nativeDefaultWalletHash
            }
        token = Contract_native_contract_id KNativeContractNativeToken
    genesisExecutionPrerequisites config
      `shouldBe` [ PrereqContract root
                 , PrereqDeployment kGenesisRootContractAddress (makeDeployedContractHash root)
                 , PrereqAuthData kGenesisRootContractAddress (AddressAuthData (replicate 21 Nothing) 0 0)
                 , PrereqContract token
                 , PrereqDeployment kNativeTokenContractAddress (makeDeployedContractHash token)
                 , PrereqContract (Contract_native_contract_id KNativeContractDefaultWallet)
                 ]

------------------------------------------------------------------------------
-- syscall_arg_params.h

syscallArgSpec :: Spec
syscallArgSpec = describe "SyscallArgParams" $ do
  it "narrows unsigned slots" $ do
    narrowSlot @Word8 255 `shouldBe` Just 255
    narrowSlot @Word8 256 `shouldBe` Nothing
    narrowSlot @Word16 65535 `shouldBe` Just 65535
    narrowSlot @Word16 65536 `shouldBe` Nothing
    narrowSlot @Word32 maxBound `shouldBe` Just maxBound
  it "requires sign extension for narrow signed slots" $ do
    narrowSlot @Int8 0x7f `shouldBe` Just 127
    narrowSlot @Int8 0xffffff80 `shouldBe` Just (-128)
    narrowSlot @Int8 0x80 `shouldBe` Nothing
    narrowSlot @Int8 0xffffff7f `shouldBe` Nothing
    narrowSlot @Int8 0x100 `shouldBe` Nothing
    narrowSlot @Int16 0xffff8000 `shouldBe` Just minBound
    narrowSlot @Int16 0x8000 `shouldBe` Nothing
  it "accepts every 32-bit pattern for a 32-bit type" $
    property $ \w -> narrowSlot @Int32 w === Just (fromIntegral w)
  it "rejects a mock slot past 32 bits" $ do
    narrowSlotMock @Word32 (fromIntegral (maxBound :: Word32)) `shouldBe` Just maxBound
    narrowSlotMock @Word32 (bit 32) `shouldBe` Nothing
    narrowSlotMock @Int8 0xffffff80 `shouldBe` Just (-128)
  it "parses bools" $ do
    map parseBoolSlot [0, 1, 2, maxBound] `shouldBe` [Just False, Just True, Nothing, Nothing]
    map parseBoolSlotMock [0, 1, bit 32] `shouldBe` [Just False, Just True, Nothing]
  it "counts slots and sentinels" $ do
    map kNumInputs [minBound .. maxBound] `shouldBe` [2, 2, 1, 1, 1]
    kOptionalInputNullSentinel `shouldBe` 0xffffffff
    kOptionalInputNullSentinelMock `shouldBe` maxBound

------------------------------------------------------------------------------
-- syscall_meta.h / syscall_inner.h (generated)

syscallMetaSpec :: Spec
syscallMetaSpec = describe "SyscallMeta" $ do
  let get n = maybe (error ("no syscall " ++ n)) id (syscallMetaByName n)
  it "has one entry per syscall, in number order" $ do
    length syscallMetaTable `shouldBe` kSyscallCount
    map (.syscallNo) syscallMetaTable `shouldBe` map SyscallNo [0 .. kSyscallCount - 1]
    map (.syscallNo) syscallMetaTable `shouldBe` syscallNo_values
    syscallMeta (SyscallNo 39) `shouldBe` Nothing
    syscallMeta (SyscallNo (-1)) `shouldBe` Nothing
    fmap (.name) (syscallMeta KSHA256Copy) `shouldBe` Just "SHA256Copy"
  it "fits every syscall in the argument slots" $ do
    kSyscallInputCount `shouldBe` 7
    mapM_ (\m -> syscallArgSlots m `shouldSatisfy` (<= kSyscallInputCount)) syscallMetaTable
  it "matches syscall_meta.h" $ do
    let sha = get "SHA256"
    (sha.isReadOnly, sha.canReturn, sha.needsContext, sha.needsGas, sha.isAsync)
      `shouldBe` (True, True, True, True, False)
    map (.name) sha.inputs `shouldBe` ["buffer"]
    map (.name) sha.outputs `shouldBe` ["hash"]
    (get "GetMsgSender").allowedErrors `shouldBe` [KSyscallErrorCodeIsRootTransaction]
    (get "Invoke").allowedErrors
      `shouldBe` [KSyscallErrorCodeInvalidSyscallInput, KSyscallErrorCodeReentrance, KSyscallErrorCodeInvokeDepthExceeded]
    (get "Return").canReturn `shouldBe` False
    (get "Exit").canReturn `shouldBe` False
    (get "Exit").needsContext `shouldBe` False
    (get "ConsoleLog").needsContext `shouldBe` False
    (get "WriteAddressRequiredWeight").isReadOnly `shouldBe` False
  it "matches the inner signatures of syscall_inner.h" $ do
    map (.kind) (get "Invoke").inputs `shouldBe` [ArgRef, ArgSpan, ArgSpan, ArgRef]
    (get "Invoke").isAsync `shouldBe` True
    map (.kind) (get "GetDeployAddress").inputs `shouldBe` [ArgOptionalRef, ArgRef, ArgRef]
    map (.kind) (get "NonnegativeIntSetAdd").inputs `shouldBe` [ArgRef, ArgOptionalRef, ArgRef]
    map (.kind) (get "NonnegativeIntGet").outputs `shouldBe` [ArgRef, ArgRef, ArgRef]
    map (.kind) (get "Return").inputs `shouldBe` [ArgSpan, ArgValue, ArgSpan]
    map (.kind) (get "WriteAddressRequiredWeight").inputs `shouldBe` [ArgValue]
    map (.typename) (get "GetBlockHeight").outputs `shouldBe` [Just "BlockHeight"]
    map (\m -> m.name) (filter (.isAsync) syscallMetaTable)
      `shouldBe` [ "Invoke"
                 , "DeployContract"
                 , "BytestringGetLen"
                 , "BytestringGet"
                 , "BytestringSet"
                 , "NonnegativeIntGet"
                 , "NonnegativeIntSetAdd"
                 , "DeleteObject"
                 , "RetrocausalQueueInsert"
                 , "RetrocausalQueueConsume"
                 , "ChangePublicKeyRegistration"
                 , "WriteAddressRequiredWeight"
                 , "RetrocausalQueuePeekLen"
                 ]
    map (\m -> m.name) (filter (.needsGas) syscallMetaTable)
      `shouldBe` ["SHA256", "Invoke", "VerifySignature", "BytestringSet", "GetTransactionStatistics", "SHA256Update"]
