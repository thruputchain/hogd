-- | Port of @hogd/genesis/genesis_constants.h@: the fixed addresses genesis
-- deploys at, and the storage key reserved to the native token.
module Hogd.Genesis.Constants
  ( genesisContractAddress
  , kNullContractAddress
  , kNativeTokenContractAddress
  , kGenesisRootContractAddress
  , kNativeTokenTotalSupplyKey
  ) where

import Data.Bits (shiftR)
import qualified Data.ByteString as B
import Data.Word (Word32)
import Hogd.Xdr.Genesis (GenesisContractAddresses (..), pattern KAddressGenesisRoot, pattern KAddressNativeToken, pattern KAddressNull)
import Hogd.Xdr.Types (ContractAddress, StorageKeyContractData)

-- | @genesis_contract_address@: 32 bytes, zero except the last four, which
-- hold the enum's value (as @uint32_t@) big-endian.
genesisContractAddress :: GenesisContractAddresses -> ContractAddress
genesisContractAddress (GenesisContractAddresses v) =
  let raw = fromIntegral v :: Word32
   in B.replicate 28 0
        <> B.pack [fromIntegral (raw `shiftR` 24), fromIntegral (raw `shiftR` 16), fromIntegral (raw `shiftR` 8), fromIntegral raw]

-- | All-zero address reserved to mean "no address" (cleared approval, unset
-- consumer, mint/burn). Genesis load rejects deployments here.
kNullContractAddress :: ContractAddress
kNullContractAddress = genesisContractAddress KAddressNull

kNativeTokenContractAddress :: ContractAddress
kNativeTokenContractAddress = genesisContractAddress KAddressNativeToken

kGenesisRootContractAddress :: ContractAddress
kGenesisRootContractAddress = genesisContractAddress KAddressGenesisRoot

-- | Contract-data key reserved to the native token implementation (the
-- all-zero key). Ordinary contracts cannot write storage owned by the
-- native token address.
kNativeTokenTotalSupplyKey :: StorageKeyContractData
kNativeTokenTotalSupplyKey = B.replicate 32 0
