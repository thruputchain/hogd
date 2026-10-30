-- | The default_wallet reference contract's trait, as the generated
-- built_contracts/default_wallet.h describes it.  Source of truth:
-- built_contracts/integration_test_contracts/default_wallet/src/interface.rs
-- (crate default_wallet 0.1.0).
module Hogd.BuiltContracts.DefaultWallet
  ( defaultWalletTrait
    -- * WalletError
  , WalletError (..)
  , pattern NotRootInvocation
  , pattern InvalidAuthClaim
  , pattern EmptyCallList
  , pattern TooManyCalls
  , pattern CallAborted
  , pattern CallFaulted
  , pattern ReturnDataTooLarge
  , kWalletErrorSelector
  , WalletExn (..)
    -- * Types
  , kWalletBytesMax
  , WalletCall (..)
  , walletCallC
  , walletCallsC
  , WalletInitCall (..)
  , walletInitCallC
  , walletBytesC
  , walletReturnsC
  , deployArgsC
  ) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.Int
import Data.Word
import Hogd.BuiltContracts.Codec
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Runtime.ContractTrait
import X23.Xdr

-- | @default_wallet::DefaultWallet@.  execute = 0xc3edb11fdd6d5d58,
-- deploy = 0x6be0cb0f6de0917a.
defaultWalletTrait :: ContractTrait
defaultWalletTrait =
  ContractTrait
    { ctName = "DefaultWallet"
    , ctVersion = "0.1.0"
    , ctFormat = Cxs
    , ctExportNames = map BC.pack ["execute", "deploy"]
    }

newtype WalletError = WalletError Int32
  deriving (Eq, Ord, Show)

pattern NotRootInvocation, InvalidAuthClaim, EmptyCallList, TooManyCalls, CallAborted, CallFaulted, ReturnDataTooLarge :: WalletError
pattern NotRootInvocation = WalletError 1
pattern InvalidAuthClaim = WalletError 2
pattern EmptyCallList = WalletError 3
pattern TooManyCalls = WalletError 4
pattern CallAborted = WalletError 5
pattern CallFaulted = WalletError 6
pattern ReturnDataTooLarge = WalletError 7

instance XdrType WalletError where
  xdrPut (WalletError v) = putInt32 v
  xdrGet = WalletError <$> getInt32
  cxsPut (WalletError v) = cxsPutInt32 v
  cxsGet = WalletError <$> cxsGetInt32

-- | sha256("0.1::default_wallet::WalletError")[0..8].
kWalletErrorSelector :: Word64
kWalletErrorSelector = 0x931d3297d37d1bdf

newtype WalletExn = WalletExn WalletError
  deriving (Eq, Show)

instance ExceptionSet WalletExn where
  encodeException (WalletExn e) = (kWalletErrorSelector, cxsPut e)
  exceptionDecoders = [(kWalletErrorSelector, WalletExn <$> cxsGet)]

-- | @WalletBytes = BytesM@: @x23::Vec<uint8_t, 4294967295>@.
kWalletBytesMax :: Word32
kWalletBytesMax = 0xffffffff

walletBytesC :: Codec B.ByteString
walletBytesC = opaqueVecC kWalletBytesMax

-- | @Vec<WalletBytes>@, execute's ok type.
walletReturnsC :: Codec [B.ByteString]
walletReturnsC = vecC 0xffffffff walletBytesC

data WalletCall = WalletCall
  { wcTarget :: B.ByteString
  , wcEntrypoint :: B.ByteString
  , wcGasLimit :: Word64
  , wcCalldata :: B.ByteString
  }
  deriving (Eq, Show)

walletCallC :: Codec WalletCall
walletCallC =
  mapC
    (\(t, e, g, c) -> WalletCall t e g c)
    (\(WalletCall t e g c) -> (t, e, g, c))
    (quadC addressC entrypointC u64C walletBytesC)

-- | execute's argument: @x23::Vec<WalletCall, 4294967295>@.
walletCallsC :: Codec [WalletCall]
walletCallsC = vecC 0xffffffff walletCallC

data WalletInitCall = WalletInitCall
  { wicEntrypoint :: B.ByteString
  , wicGasLimit :: Word64
  , wicCalldata :: B.ByteString
  }
  deriving (Eq, Show)

walletInitCallC :: Codec WalletInitCall
walletInitCallC =
  mapC
    (\(e, g, c) -> WalletInitCall e g c)
    (\(WalletInitCall e g c) -> (e, g, c))
    (tripleC entrypointC u64C walletBytesC)

-- | deploy's arguments: (nonce, contract_hash, amount, initializer).
deployArgsC :: Codec (B.ByteString, B.ByteString, Integer, Maybe WalletInitCall)
deployArgsC = quadC hashC hashC i128C (optionC walletInitCallC)
