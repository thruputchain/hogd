-- | The parts of @hogd/genesis/genesis_configuration.{h,cc}@ that hog_exec
-- and its test fixtures rely on: precompile addresses, the native default
-- wallet's deployed hash, genesis deployment addresses, the stock network
-- configuration, and the execution prerequisites a genesis installs.
--
-- Not ported: @GenesisConfigurationBuilder@ (the height-1 ceremony
-- transactions need the genesis_root contract client and artifact) and
-- @load_stock_genesis_root_contract@ (reads built_contracts/).
module Hogd.Genesis.Configuration
  ( GenesisCeremonyError (..)
  , makePrecompileAddress
  , sha256Xdr
  , makeNativeContract
  , makeDeployedContractHash
  , nativeDefaultWalletHash
  , calculateGenesisDeploymentAddress
  , stockNetworkConfig
  , GenesisPrerequisite (..)
  , genesisExecutionPrerequisites
  ) where

import qualified Crypto.Hash as H
import qualified Data.ByteArray as BA
import qualified Data.ByteString as B
import Hogd.Base.NetworkConfigParams (statedDefaults)
import Hogd.Genesis.Constants
import Hogd.Xdr.AddressAuth (AddressAuthData (..))
import Hogd.Xdr.AuthClaim (kMaxSigners)
import Hogd.Xdr.Contract
import Hogd.Xdr.Genesis (GenesisConfiguration (..), GenesisContractAddresses)
import Hogd.Xdr.NetworkConfig (NetworkConfig)
import Hogd.Xdr.Precompiles
import Hogd.Xdr.Types (ContractAddress, SHA256Hash)
import X23.Xdr (XdrType, xdrEncodeStrict)

-- | @GenesisCeremonyError@.
data GenesisCeremonyError
  = KNegativeDistribution
  | KSupplyExceeded
  | -- | Too many distinct wallet artifacts among the accounts funded together.
    KTooManyDistinctContracts
  deriving (Eq, Show)

-- | @make_precompile_address@: identical to 'genesisContractAddress'.
makePrecompileAddress :: GenesisContractAddresses -> ContractAddress
makePrecompileAddress = genesisContractAddress

-- | @sha256(const T &)@ (hogd/crypto/crypto.h): SHA-256 of the XDR
-- serialization.
sha256Xdr :: XdrType a => a -> SHA256Hash
sha256Xdr x = BA.convert (H.hash (xdrEncodeStrict x) :: H.Digest H.SHA256)

-- | @make_native_contract@.
makeNativeContract :: HogexecPrecompiledNativeContracts -> Contract
makeNativeContract = Contract_native_contract_id

-- | @make_deployed_contract_hash@: native and mock contracts have size 0,
-- an RV32 contract its blob's size (narrowed to @uint32_t@).
makeDeployedContractHash :: Contract -> DeployedContractHash
makeDeployedContractHash contract =
  DeployedContractHash
    { hash = sha256Xdr contract
    , contract_size = case contract of
        Contract_rv32_contract blob -> fromIntegral (B.length blob.data_)
        _ -> 0
    , contract_type = discriminant_Contract contract
    }

-- | @native_default_wallet_hash()@: the default
-- @GenesisConfiguration::default_wallet@.
nativeDefaultWalletHash :: DeployedContractHash
nativeDefaultWalletHash = makeDeployedContractHash (makeNativeContract KNativeContractDefaultWallet)

-- | @calculate_genesis_deployment_address@: the address the genesis root
-- deploys @contract_hash@ at under @nonce@.
calculateGenesisDeploymentAddress :: SHA256Hash -> SHA256Hash -> ContractAddress
calculateGenesisDeploymentAddress contractHash nonce =
  sha256Xdr
    ContractDeploymentHashPreimage
      { deployer_address = kGenesisRootContractAddress
      , nonce = nonce
      , contract_hash = contractHash
      }

-- | @stock_network_config()@: this repository's dev and test chains run at
-- the protocol defaults, every parameter stated outright.
stockNetworkConfig :: NetworkConfig
stockNetworkConfig = statedDefaults

-- | One record @visit_genesis_execution_prerequisites@ visits.
data GenesisPrerequisite
  = PrereqContract Contract
  | PrereqDeployment ContractAddress DeployedContractHash
  | PrereqAuthData ContractAddress AddressAuthData
  deriving (Eq, Show)

-- | @visit_genesis_execution_prerequisites@, as the list of visits in the
-- order the C++ makes them: the root contract, its deployment and root auth
-- record; the native token and its deployment; the native default wallet
-- (code only).
genesisExecutionPrerequisites :: GenesisConfiguration -> [GenesisPrerequisite]
genesisExecutionPrerequisites config =
  [ PrereqContract root
  , PrereqDeployment kGenesisRootContractAddress (makeDeployedContractHash root)
  , PrereqAuthData kGenesisRootContractAddress rootAuth
  , PrereqContract token
  , PrereqDeployment kNativeTokenContractAddress (makeDeployedContractHash token)
  , PrereqContract (makeNativeContract KNativeContractDefaultWallet)
  ]
  where
    root = config.root_contract
    token = makeNativeContract KNativeContractNativeToken
    -- AddressAuthData{} with required_weight and last_written_height zeroed.
    rootAuth =
      AddressAuthData
        { public_keys = replicate kMaxSigners Nothing
        , required_weight = 0
        , last_written_height = 0
        }
