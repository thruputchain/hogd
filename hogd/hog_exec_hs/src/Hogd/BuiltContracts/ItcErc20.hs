-- | The contract traits the native token implements, as the generated
-- built_contracts/itc_erc20.h and itc_erc20_permit.h describe them.
--
-- Those headers are generated at build time from the contracts' spec JSON
-- (generate_header.py).  The source of truth here is the interface crates
-- the specs come from: @thruput-erc20-if@, @thruput-erc20-permit-if@ and
-- @thruput-erc165-if@, all at version 0.1.71 in built_contracts/Cargo.lock.
-- A trait's version is its declaring crate's @CARGO_PKG_VERSION@ (the
-- @#[contract]@ macro), so every entrypoint id hashes @"<Trait>::0.1::..."@.
-- Exception selectors are @sha256("<major>::<crate>::<Type>")@ of the
-- declaring crate.
module Hogd.BuiltContracts.ItcErc20
  ( -- * Traits
    erc20Trait
  , erc20PermitTrait
  , erc165Trait
    -- * ERC20Error
  , ERC20Error (..)
  , pattern AlreadyInitialized
  , pattern MustBeContractOwner
  , pattern TooLarge
  , pattern BalanceNotZero
  , pattern NegativeAmount
  , pattern Internal
  , kErc20ErrorSelector
  , Erc20Exn (..)
    -- * ERC20PermitError
  , ERC20PermitError (..)
  , pattern NotSpender
  , pattern ExceedsGrant
  , kErc20PermitErrorSelector
  , PermitExn (..)
    -- * Grants
  , PermitGrant (..)
  , permitGrantC
  , DeleteAllowanceGrant (..)
  , deleteAllowanceGrantC
    -- * ERC165
  , kInterfaceQueryMax
  , interfaceIdC
  , interfaceIdsC
  , interfaceSupportC
  ) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.Int
import Data.Word
import Hogd.BuiltContracts.Codec
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Runtime.ContractTrait
import X23.Xdr

-- | @itc_erc20::ERC20@ (thruput-erc20-if).
erc20Trait :: ContractTrait
erc20Trait =
  ContractTrait
    { ctName = "ERC20"
    , ctVersion = "0.1.71"
    , ctFormat = Cxs
    , ctExportNames =
        map
          BC.pack
          [ "initialize"
          , "transfer"
          , "transfer_from"
          , "adjust_allowance"
          , "delete_allowance"
          , "delete_balance"
          , "allowance"
          , "get_balance_of"
          , "get_balance"
          , "mint"
          , "total_supply"
          ]
    }

-- | @itc_erc20_permit::ERC20Permit@ (thruput-erc20-permit-if).
erc20PermitTrait :: ContractTrait
erc20PermitTrait =
  ContractTrait
    { ctName = "ERC20Permit"
    , ctVersion = "0.1.71"
    , ctFormat = Cxs
    , ctExportNames = map BC.pack ["transfer_from_signed", "increase_allowance_signed", "delete_allowance_signed"]
    }

-- | @itc_erc20_permit::ERC165@ (thruput-erc165-if).  Declares no
-- exceptions, so its methods return plain values.
erc165Trait :: ContractTrait
erc165Trait =
  ContractTrait
    { ctName = "ERC165"
    , ctVersion = "0.1.71"
    , ctFormat = Cxs
    , ctExportNames = map BC.pack ["supports_interface", "supports_interfaces"]
    }

-- | @ERC20Error@ (an open enum, int32 on the wire).
newtype ERC20Error = ERC20Error Int32
  deriving (Eq, Ord, Show)

pattern AlreadyInitialized, MustBeContractOwner, TooLarge, BalanceNotZero, NegativeAmount, Internal :: ERC20Error
pattern AlreadyInitialized = ERC20Error 1
pattern MustBeContractOwner = ERC20Error 2
pattern TooLarge = ERC20Error 3
pattern BalanceNotZero = ERC20Error 4
pattern NegativeAmount = ERC20Error 5
pattern Internal = ERC20Error 100

instance XdrType ERC20Error where
  xdrPut (ERC20Error v) = putInt32 v
  xdrGet = ERC20Error <$> getInt32
  cxsPut (ERC20Error v) = cxsPutInt32 v
  cxsGet = ERC20Error <$> cxsGetInt32

-- | sha256("0.1::thruput-erc20-if::ERC20Error")[0..8].
kErc20ErrorSelector :: Word64
kErc20ErrorSelector = 0xf63c8b1f5e7e2fe0

-- | The ERC20 interface's exception set.
newtype Erc20Exn = Erc20Exn ERC20Error
  deriving (Eq, Show)

instance ExceptionSet Erc20Exn where
  encodeException (Erc20Exn e) = (kErc20ErrorSelector, cxsPut e)
  exceptionDecoders = [(kErc20ErrorSelector, Erc20Exn <$> cxsGet)]

newtype ERC20PermitError = ERC20PermitError Int32
  deriving (Eq, Ord, Show)

pattern NotSpender, ExceedsGrant :: ERC20PermitError
pattern NotSpender = ERC20PermitError 1
pattern ExceedsGrant = ERC20PermitError 2

instance XdrType ERC20PermitError where
  xdrPut (ERC20PermitError v) = putInt32 v
  xdrGet = ERC20PermitError <$> getInt32
  cxsPut (ERC20PermitError v) = cxsPutInt32 v
  cxsGet = ERC20PermitError <$> cxsGetInt32

-- | sha256("0.1::thruput-erc20-permit-if::ERC20PermitError")[0..8].
kErc20PermitErrorSelector :: Word64
kErc20PermitErrorSelector = 0x1aaf2b9becb7ddc4

-- | The ERC20Permit interface's exception set, in the generated header's
-- order (ERC20PermitError, then ERC20Error).
data PermitExn
  = PermitErr ERC20PermitError
  | PermitErc20Err ERC20Error
  deriving (Eq, Show)

instance ExceptionSet PermitExn where
  encodeException = \case
    PermitErr e -> (kErc20PermitErrorSelector, cxsPut e)
    PermitErc20Err e -> (kErc20ErrorSelector, cxsPut e)
  exceptionDecoders =
    [ (kErc20PermitErrorSelector, PermitErr <$> cxsGet)
    , (kErc20ErrorSelector, PermitErc20Err <$> cxsGet)
    ]

-- | @PermitGrant { spender, amount }@.
data PermitGrant = PermitGrant
  { pgSpender :: B.ByteString
  , pgAmount :: Integer
  }
  deriving (Eq, Show)

permitGrantC :: Codec PermitGrant
permitGrantC = mapC (uncurry PermitGrant) (\g -> (pgSpender g, pgAmount g)) (pairC addressC i128C)

-- | @DeleteAllowanceGrant { spender }@.
newtype DeleteAllowanceGrant = DeleteAllowanceGrant {dagSpender :: B.ByteString}
  deriving (Eq, Show)

deleteAllowanceGrantC :: Codec DeleteAllowanceGrant
deleteAllowanceGrantC = mapC DeleteAllowanceGrant dagSpender addressC

-- | @INTERFACE_QUERY_MAX@.
kInterfaceQueryMax :: Word32
kInterfaceQueryMax = 32

-- | @InterfaceId@: @std::array<uint8_t, 8>@.
interfaceIdC :: Codec B.ByteString
interfaceIdC = fixedOpaqueC 8

-- | @InterfaceIds@: @heapless::Vec<InterfaceId, 32>@.
interfaceIdsC :: Codec [B.ByteString]
interfaceIdsC = vecC kInterfaceQueryMax interfaceIdC

-- | @InterfaceSupport@: @heapless::Vec<bool, 32>@.
interfaceSupportC :: Codec [Bool]
interfaceSupportC = vecC kInterfaceQueryMax boolC
