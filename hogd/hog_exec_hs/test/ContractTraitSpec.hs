-- | Pure tests of the precompile building blocks: entrypoint, interface and
-- selector derivation (contract_trait_test.cc, contract_result_test.cc,
-- default_wallet_parity_test.cc's id pins), abort-buffer encoding and
-- dispatch, calldata codecs and event bytes.
module ContractTraitSpec (spec) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Base16 as B16
import qualified Data.ByteString.Char8 as BC
import Data.Int (Int64)
import Data.Word (Word64)
import Hogd.BuiltContracts.Codec
import Hogd.BuiltContracts.DefaultWallet
import Hogd.BuiltContracts.ItcErc20
import Hogd.Crypto (sha256)
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Precompiles.Context (hogExecPrecompiles, makeHogExecContext)
import Hogd.Exec.Precompiles.NativeContractBuilder
import Hogd.Exec.Precompiles.PrecompileDefaultWallet (defaultWalletMethods)
import Hogd.Exec.Precompiles.PrecompileNativeToken
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.Types (RuntimeContext (..))
import Hogd.Base.NetworkConfigParams (networkConfigParamsOf)
import Hogd.Genesis.Configuration (stockNetworkConfig)
import qualified Data.Map.Strict as M
import Hogd.Xdr
import Test.Hspec
import Test.QuickCheck
import X23.Xdr (cxsPut, cxsPutWord64)
import qualified Data.ByteString.Builder as BB
import qualified Data.ByteString.Lazy as BL

hex :: String -> B.ByteString
hex s = either error id (B16.decode (BC.pack s))

toHex :: B.ByteString -> String
toHex = BC.unpack . B16.encode

build :: BB.Builder -> B.ByteString
build = BL.toStrict . BB.toLazyByteString

spec :: Spec
spec = do
  derivationSpec
  traitSpec
  contractResultSpec
  codecSpec
  eventSpec
  contextSpec

derivationSpec :: Spec
derivationSpec = describe "sdk_entrypoint_derivation" $ do
  -- contract_trait_test.cc: MethodIDDerivation
  it "folds the version to its major component (golden vectors)" $ do
    let mid = sdkMethodIdFor "hogd"
    mid "0.1.0" "abc" `shouldBe` 0x9432b95f6309d52b
    mid "0.1.12" "abc" `shouldBe` mid "0.1.0" "abc"
    mid "0.1.0-alpha" "abc" `shouldBe` mid "0.1.0" "abc"
    mid "0.1.0" "def" `shouldNotBe` mid "0.1.0" "abc"
    mid "0.0.0-alpha" "abc" `shouldBe` 0x99b536b207ea02cf
    mid "0.0.0-alpha" "abc" `shouldNotBe` mid "0.0.0-beta" "abc"
    mid "0.0.0-alpha" "abc" `shouldNotBe` mid "0.0.0" "abc"
    mid "2.5.7" "abc" `shouldBe` 0x69e35d2fdb6e7e55
    mid "2.9.9" "abc" `shouldBe` mid "2.5.7" "abc"
    mid "3.0.0" "abc" `shouldNotBe` mid "2.5.7" "abc"
    mid "0.0.7" "abc" `shouldBe` 0xa6e0c6d93a7f2c46
    mid "0.0.7-rc.1" "abc" `shouldBe` mid "0.0.7" "abc"
    mid "0.0.8" "abc" `shouldNotBe` mid "0.0.7" "abc"

  it "majorVersion keeps a prefix of the version" $ do
    majorVersion "1.2.3" `shouldBe` "1"
    majorVersion "0.9.2" `shouldBe` "0.9"
    majorVersion "0.0.2" `shouldBe` "0.0.2"
    majorVersion "0.0.0+build" `shouldBe` "0.0.0+build"
    majorVersion "10.0.0" `shouldBe` "10"

  -- contract_trait_test.cc: EntrypointFromMethodID
  it "entrypoint_from_method_id inverts sdk_method_id" $
    sdkMethodId (entrypointFromMethodId 0x1234567890ABCDEF) `shouldBe` 0x1234567890ABCDEF
  it "entrypoint_from_method_id inverts sdk_method_id (property)" $
    property $ \(w :: Word64) -> sdkMethodId (entrypointFromMethodId w) === w

  -- contract_trait_test.cc: EntrypointDebugCrossCheckDerivesFromUtf8Metadata
  it "matches the UTF-8-derived ERC20 transfer id" $
    sdkMethodIdFor "ERC20" "0.1.4" "transfer" `shouldBe` 0x63285b3523f9dc3f

  it "derives interface ids from thruput-interface::<name>::<major>" $ do
    sdkInterfaceIdDerivation "ERC20" "0.1.71" `shouldBe` B.take 8 (sha256 "thruput-interface::ERC20::0.1")
    toHex (traitInterfaceId erc20Trait) `shouldBe` "c61997e5212a0b6f"
    toHex (traitInterfaceId erc20PermitTrait) `shouldBe` "30d5dae4b876696c"
    toHex (traitInterfaceId erc165Trait) `shouldBe` "2aa0dc3698a02913"

traitSpec :: Spec
traitSpec = describe "generated traits" $ do
  -- default_wallet_parity_test.cc: ConstructsAndCoversAllEntrypoints
  it "pins the default wallet's method ids" $ do
    traitMethodId defaultWalletTrait "execute" `shouldBe` 0xc3edb11fdd6d5d58
    traitMethodId defaultWalletTrait "deploy" `shouldBe` 0x6be0cb0f6de0917a
    map fst defaultWalletMethods `shouldBe` [0xc3edb11fdd6d5d58, 0x6be0cb0f6de0917a]

  it "keys the native token by the ERC20, ERC20Permit and ERC165 ids" $ do
    traitMethodId erc20Trait "transfer" `shouldBe` 0x63285b3523f9dc3f
    let ids = map fst nativeTokenMethods
    length ids `shouldBe` 16
    checkMethods erc20Trait nativeTokenMethods `shouldBe` True
    checkMethods erc20PermitTrait nativeTokenMethods `shouldBe` True
    checkMethods erc165Trait nativeTokenMethods `shouldBe` True
    M.size (M.fromList (map (\i -> (i, ())) ids)) `shouldBe` 16

  it "names every ERC20 method by sha256(\"ERC20::0.1::<method>\")" $
    mapM_
      (\m -> traitMethodId erc20Trait m `shouldBe` sdkMethodId (B.take 8 (sha256 ("ERC20::0.1::" <> m))))
      (ctExportNames erc20Trait)

contractResultSpec :: Spec
contractResultSpec = describe "contract results" $ do
  -- contract_result_test.cc: LibraryExceptionSelectorMatchesDerivation
  it "derives the library exception selector" $
    calculateSelector "1" "sdk" "LibraryExn" `shouldBe` kLibraryExceptionSelector

  it "derives the interface exception selectors" $ do
    calculateSelector "0.1" "thruput-erc20-if" "ERC20Error" `shouldBe` kErc20ErrorSelector
    calculateSelector "0.1" "thruput-erc20-permit-if" "ERC20PermitError" `shouldBe` kErc20PermitErrorSelector
    calculateSelector "0.1" "default_wallet" "WalletError" `shouldBe` kWalletErrorSelector

  -- contract_result_test.cc: RawAbortPreservesKnownSelectorAndOpaqueBody
  it "keeps a known selector with an undecodable body raw" $ do
    let wire = build (cxsPutWord64 kLibraryExceptionSelector) <> B.singleton 0xff
        err = fromAbortBuffer wire :: ContractErrorType NoExceptions
    err `shouldBe` ErrRawAbort wire
    serializeContractError err `shouldBe` wire

  it "encodes the DecodingArgs abort exactly" $ do
    toHex decodingArgsAbortBytes `shouldBe` "ffc30ac65990d562e50100"
    (fromAbortBuffer decodingArgsAbortBytes :: ContractErrorType NoExceptions)
      `shouldBe` ErrLibrary (LibraryExn_handling HandlingError_kHandlingDecodingArgs)

  it "encodes interface exceptions as (u64 selector, CXS body)" $ do
    toHex (serializeContractError (ErrException (WalletExn InvalidAuthClaim))) `shouldBe` "ffdf1b7dd397321d9302"
    toHex (serializeContractError (ErrException (Erc20Exn TooLarge))) `shouldBe` "ffe02f7e5e1f8b3cf603"

  it "dispatches by selector in declaration order and round-trips" $ do
    let permitBytes = serializeContractError (ErrException (PermitErr ExceedsGrant))
        erc20Bytes = serializeContractError (ErrException (Erc20Exn NegativeAmount))
    fromAbortBuffer permitBytes `shouldBe` ErrException (PermitErr ExceedsGrant)
    fromAbortBuffer erc20Bytes `shouldBe` ErrException (PermitErc20Err NegativeAmount)
    (fromAbortBuffer erc20Bytes :: ContractErrorType WalletExn) `shouldBe` ErrRawAbort erc20Bytes
    (fromAbortBuffer (erc20Bytes <> "x") :: ContractErrorType Erc20Exn) `shouldBe` ErrRawAbort (erc20Bytes <> "x")
    (fromAbortBuffer "" :: ContractErrorType Erc20Exn) `shouldBe` ErrRawAbort ""

  it "encodes syscall library exceptions" $ do
    let exn = syscallException KDeployContract KSyscallErrorCodeInvalidSyscallInput
        SyscallNo callNo = KDeployContract
        SyscallErrorCode code = KSyscallErrorCodeInvalidSyscallInput
    exn `shouldBe` LibraryExn_syscall_exn SyscallExn {call_number = fromIntegral callNo, err_number = code}
    B.take 10 (libraryExceptionBytes exn) `shouldBe` hex "ffc30ac65990d562e500"

codecSpec :: Spec
codecSpec = describe "calldata codecs" $ do
  it "encodes i128 as (int64 high, uint64 low)" $ do
    toHex (encodeC Cxs i128C 500000) `shouldBe` "00bf20a10700"
    toHex (encodeC Txs i128C (-1)) `shouldBe` "ffffffffffffffffffffffffffffffff"
    toHex (encodeC Cxs i128C 0) `shouldBe` "0000"
  it "round-trips i128 in both formats" $
    property $ \(hi :: Int64) (lo :: Word64) ->
      let v = i128Join hi lo
       in decodeC Cxs i128C (encodeC Cxs i128C v) === Just v .&&. decodeC Txs i128C (encodeC Txs i128C v) === Just v

  it "rejects trailing calldata" $ do
    let a = B.replicate 32 7
    decodeC Cxs addressC a `shouldBe` Just a
    decodeC Cxs addressC (a <> "\0") `shouldBe` Nothing

  it "round-trips wallet calls" $ do
    let calls = [WalletCall (B.replicate 32 1) (B.replicate 8 2) maxBound "abc", WalletCall (B.replicate 32 3) (B.replicate 8 4) 5 ""]
    decodeC Cxs walletCallsC (encodeC Cxs walletCallsC calls) `shouldBe` Just calls

  it "keys allowances by the TXS encoding of (owner, spender)" $ do
    let o = B.replicate 32 1
        s = B.replicate 32 2
    allowanceKeyPreimage o s `shouldBe` o <> s

eventSpec :: Spec
eventSpec = describe "native token events" $ do
  it "uses the SDK's thruput-erc20-if event signatures" $ do
    nativeTokenTransferEventSignature `shouldBe` sha256 "0.1::thruput-erc20-if::TransferEvent"
    nativeTokenAllowanceChangeEventSignature `shouldBe` sha256 "0.1::thruput-erc20-if::AllowanceChangeEvent"
    nativeTokenMintEventSignature `shouldBe` sha256 "0.1::thruput-erc20-if::MintEvent"
    nativeTokenAllowanceDeleteEventSignature `shouldBe` sha256 "0.1::thruput-erc20-if::AllowanceDeleteEvent"

  it "lays out events as signature, address topics, CXS payload" $ do
    let from = B.replicate 32 0xaa
        to = B.replicate 32 0xbb
    transferEventBytes from to 500000 `shouldBe` nativeTokenTransferEventSignature <> from <> to <> hex "00bf20a10700"
    mintEventBytes to 1 `shouldBe` nativeTokenMintEventSignature <> to <> hex "0001"
    allowanceChangeEventBytes from to (-1) `shouldBe` nativeTokenAllowanceChangeEventSignature <> from <> to <> hex "3effffffffffffffffff"
    allowanceDeleteEventBytes from to 0 `shouldBe` nativeTokenAllowanceDeleteEventSignature <> from <> to <> hex "0000"

  it "matches the standard asset TransferEvent CXS encoding" $ do
    let from = B.replicate 32 0x11
        to = B.replicate 32 0x22
        ev = TransferEvent {from_address = from, to_address = to, amount = X_int128_xdr {highbits = 0, lowbits = 77}}
    transferEventBytes from to 77 `shouldBe` nativeTokenTransferEventSignature <> build (cxsPut ev)

contextSpec :: Spec
contextSpec = describe "make_hog_exec_context" $ do
  it "registers the native token and the default wallet" $
    M.keys hogExecPrecompiles `shouldBe` [KNativeContractNativeToken, KNativeContractDefaultWallet]

  it "verifies precompiles and rejects unknown native ids and absent rv32" $ do
    let params = networkConfigParamsOf stockNetworkConfig
        verify c = either (Just . fst) (const Nothing) <$> rcVerifyContract makeHogExecContext c params
    verify (Contract_native_contract_id KNativeContractNativeToken) `shouldReturn` Nothing
    verify (Contract_native_contract_id KNativeContractDefaultWallet) `shouldReturn` Nothing
    verify (Contract_native_contract_id (HogexecPrecompiledNativeContracts 1)) `shouldReturn` Just Deterministic
    verify (Contract_mock_contract_id 1) `shouldReturn` Just Deterministic
