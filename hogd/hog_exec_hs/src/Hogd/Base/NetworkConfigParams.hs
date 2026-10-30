{-# LANGUAGE NoFieldSelectors #-}

-- | Port of @hogd/base/network_config_params.h@: reading the parameters a
-- chain's configuration states.
--
-- A 'NetworkConfig' states parameters as a list of 'ConfigParam' arms whose
-- ids run 0, 1, 2, ... ("canonical form"). 'NetworkConfigParams' is that
-- list flattened, with every parameter the list does not reach at its
-- default. The defaults are consensus-critical.
--
-- C++ @CHECK@ failures (a non-canonical list reaching the constructor, a
-- value too wide for a uint32 arm, an unknown id) are 'error' calls here:
-- they are bugs in this node, not untrusted-input rejections. Callers that
-- take a configuration from a peer test 'isCanonical' first, as the C++
-- does (@config_hashable@).
module Hogd.Base.NetworkConfigParams
  ( -- * Defaults
    kDefaultMaxSHA256Buffers
  , kDefaultMaxCrossInvokeDepth
  , kDefaultMaxNewContractsPerTx
  , kDefaultMaxAuthTokens
  , kDefaultMaxCalldataLen
  , kDefaultMaxReturnBufferLen
  , kDefaultMaxTxGas
    -- * Parameters in force
  , NetworkConfigParams (..)
  , defaultNetworkConfigParams
  , networkConfigParams
  , networkConfigParamsOf
  , inForce
  , statedParam
  , isCanonical
  , configHashable
  , setParam
  , makeParam
  , statedDefaults
  , toNetworkConfig
    -- * Parameter ids
  , kHighestConfigParamID
  , allConfigParamIDs
  , configParamName
  , configParamId
  , configParamMaxValue
  , configParamValue
  ) where

import Data.Int (Int32)
import Data.Word (Word32, Word64)
import Hogd.Xdr.AuthToken (kAuthTokenMaxDepth)
import Hogd.Xdr.Contract (kDefaultMaxContractSizeBytes)
import Hogd.Xdr.HogExecConstants
  ( kDefaultMaxRv32CodeBytes
  , kDefaultMaxRv32MemoryBytes
  , kMaxSavepointDepth
  , kSHA256BufferMaxSize
  )
import Hogd.Xdr.NetworkConfig
import Hogd.Xdr.StorageItem (kDefaultMaxBytestringLen)
import Hogd.Xdr.Types (GasAmount)

-- | Values in force for parameters a configuration does not state.
kDefaultMaxSHA256Buffers :: Word32
kDefaultMaxSHA256Buffers = 32

kDefaultMaxCrossInvokeDepth :: Word32
kDefaultMaxCrossInvokeDepth = 16

-- | Not permissive: a chain predating the parameter would otherwise attach
-- contracts without bound (#2190).
kDefaultMaxNewContractsPerTx :: Word32
kDefaultMaxNewContractsPerTx = 4

-- | Unbounded before the parameter (#4174, #4040).
kDefaultMaxAuthTokens :: Word32
kDefaultMaxAuthTokens = 64

-- | Matches guest memory, so it bounds a root invocation's calldata without
-- failing any sub-invoke that runs today.
kDefaultMaxCalldataLen :: Word32
kDefaultMaxCalldataLen = 65536

-- | The value the wire type fixed before the parameter existed.
kDefaultMaxReturnBufferLen :: Word32
kDefaultMaxReturnBufferLen = 1024

-- | Not permissive: a chain that does not state a budget cap gets this one.
kDefaultMaxTxGas :: GasAmount
kDefaultMaxTxGas = 1000000000

-- | The parameters in force, flattened from the list that states them.
-- Fields are strict, so building one performs every check the C++
-- constructor does.
data NetworkConfigParams = NetworkConfigParams
  { max_sha256_buffers :: !Word32
  , max_cross_invoke_depth :: !Word32
  , max_new_contracts_per_tx :: !Word32
  , max_bytestring_len :: !Word32
  , max_contract_size_bytes :: !Word32
  , max_savepoint_depth :: !Word32
  , max_sha256_buffer_bytes :: !Word32
  , max_auth_token_depth :: !Word32
  , max_rv32_code_bytes :: !Word32
  , max_rv32_memory_bytes :: !Word32
  , max_auth_tokens :: !Word32
  , max_calldata_len :: !Word32
  , max_return_buffer_len :: !Word32
  , max_tx_gas :: !GasAmount
  }
  deriving (Eq, Show)

-- | @NetworkConfigParams()@: the defaults in force, what an empty list
-- resolves to.
defaultNetworkConfigParams :: NetworkConfigParams
defaultNetworkConfigParams =
  NetworkConfigParams
    { max_sha256_buffers = kDefaultMaxSHA256Buffers
    , max_cross_invoke_depth = kDefaultMaxCrossInvokeDepth
    , max_new_contracts_per_tx = kDefaultMaxNewContractsPerTx
    , max_bytestring_len = kDefaultMaxBytestringLen
    , max_contract_size_bytes = kDefaultMaxContractSizeBytes
    , max_savepoint_depth = kMaxSavepointDepth
    , max_sha256_buffer_bytes = kSHA256BufferMaxSize
    , max_auth_token_depth = kAuthTokenMaxDepth
    , max_rv32_code_bytes = kDefaultMaxRv32CodeBytes
    , max_rv32_memory_bytes = kDefaultMaxRv32MemoryBytes
    , max_auth_tokens = kDefaultMaxAuthTokens
    , max_calldata_len = kDefaultMaxCalldataLen
    , max_return_buffer_len = kDefaultMaxReturnBufferLen
    , max_tx_gas = kDefaultMaxTxGas
    }

-- | @explicit NetworkConfigParams(NetworkConfigV0 const &)@: the parameters
-- @config@ puts in force. Each id up to 'kHighestConfigParamID' that the
-- list reaches must sit at its own index (CHECK; see 'statedParam').
networkConfigParams :: NetworkConfigV0 -> NetworkConfigParams
networkConfigParams config =
  NetworkConfigParams
    { max_sha256_buffers = u32 KConfigParamMaxSHA256Buffers kDefaultMaxSHA256Buffers
    , max_cross_invoke_depth = u32 KConfigParamMaxCrossInvokeDepth kDefaultMaxCrossInvokeDepth
    , max_new_contracts_per_tx = u32 KConfigParamMaxNewContractsPerTx kDefaultMaxNewContractsPerTx
    , max_bytestring_len = u32 KConfigParamMaxBytestringLen kDefaultMaxBytestringLen
    , max_contract_size_bytes = u32 KConfigParamMaxContractSizeBytes kDefaultMaxContractSizeBytes
    , max_savepoint_depth = u32 KConfigParamMaxSavepointDepth kMaxSavepointDepth
    , max_sha256_buffer_bytes = u32 KConfigParamMaxSHA256BufferBytes kSHA256BufferMaxSize
    , max_auth_token_depth = u32 KConfigParamMaxAuthTokenDepth kAuthTokenMaxDepth
    , max_rv32_code_bytes = u32 KConfigParamMaxRv32CodeBytes kDefaultMaxRv32CodeBytes
    , max_rv32_memory_bytes = u32 KConfigParamMaxRv32MemoryBytes kDefaultMaxRv32MemoryBytes
    , max_auth_tokens = u32 KConfigParamMaxAuthTokens kDefaultMaxAuthTokens
    , max_calldata_len = u32 KConfigParamMaxCalldataLen kDefaultMaxCalldataLen
    , max_return_buffer_len = u32 KConfigParamMaxReturnBufferLen kDefaultMaxReturnBufferLen
    , max_tx_gas = maybe kDefaultMaxTxGas id (statedParam config KConfigParamMaxTxGas)
    }
  where
    u32 pid def = toU32 (maybe (fromIntegral (def :: Word32)) id (statedParam config pid))

-- | 'networkConfigParams' of a 'NetworkConfig' (@NetworkConfigParams(config.v0())@).
networkConfigParamsOf :: NetworkConfig -> NetworkConfigParams
networkConfigParamsOf (NetworkConfig_v0 v0) = networkConfigParams v0

-- | @stated_param@: the value the list states for @id@, or 'Nothing' past
-- its end. Canonical form puts it at its own index, so a mismatch is a bug
-- (CHECK_EQ).
statedParam :: NetworkConfigV0 -> ConfigParamID -> Maybe Word64
statedParam config pid@(ConfigParamID index)
  | index < 0 = checkFailed ("CHECK_GE(index, 0) failed: " ++ show index)
  | otherwise = case drop (fromIntegral index) config.params of
      [] -> Nothing
      (param : _) ->
        let ConfigParamID got = discriminant_ConfigParam param
         in if got /= index
              then
                checkFailed
                  ( "CHECK_EQ(param id, index) failed: param id "
                      ++ show got
                      ++ " at index "
                      ++ show index
                      ++ " ("
                      ++ show pid
                      ++ ")"
                  )
              else Just (configParamValue param)

-- | @to_u32@: narrows a uint32 arm's value (CHECK_LE against UINT32_MAX).
toU32 :: Word64 -> Word32
toU32 v
  | v > fromIntegral (maxBound :: Word32) =
      checkFailed ("CHECK_LE(value, UINT32_MAX) failed: " ++ show v)
  | otherwise = fromIntegral v

-- | @in_force@: the value in force for @id@.
inForce :: NetworkConfigParams -> ConfigParamID -> Word64
inForce p = \case
  KConfigParamMaxSHA256Buffers -> fromIntegral p.max_sha256_buffers
  KConfigParamMaxCrossInvokeDepth -> fromIntegral p.max_cross_invoke_depth
  KConfigParamMaxNewContractsPerTx -> fromIntegral p.max_new_contracts_per_tx
  KConfigParamMaxBytestringLen -> fromIntegral p.max_bytestring_len
  KConfigParamMaxContractSizeBytes -> fromIntegral p.max_contract_size_bytes
  KConfigParamMaxSavepointDepth -> fromIntegral p.max_savepoint_depth
  KConfigParamMaxSHA256BufferBytes -> fromIntegral p.max_sha256_buffer_bytes
  KConfigParamMaxAuthTokenDepth -> fromIntegral p.max_auth_token_depth
  KConfigParamMaxRv32CodeBytes -> fromIntegral p.max_rv32_code_bytes
  KConfigParamMaxRv32MemoryBytes -> fromIntegral p.max_rv32_memory_bytes
  KConfigParamMaxAuthTokens -> fromIntegral p.max_auth_tokens
  KConfigParamMaxCalldataLen -> fromIntegral p.max_calldata_len
  KConfigParamMaxReturnBufferLen -> fromIntegral p.max_return_buffer_len
  KConfigParamMaxTxGas -> p.max_tx_gas
  pid -> unknownId pid

-- | @is_canonical@: ids run 0, 1, 2, ... with no gaps. Hashed as written, so
-- one form per configuration is one hash per configuration.
isCanonical :: NetworkConfigV0 -> Bool
isCanonical config =
  and (zipWith (\expected p -> discriminant_ConfigParam p == ConfigParamID expected) [0 ..] config.params)

-- | @config_hashable@ (hogd/hog_mempool/config_hash.h): whether a
-- configuration may be hashed at all. Every path taking one from an
-- untrusted peer must check this first.
configHashable :: NetworkConfig -> Bool
configHashable (NetworkConfig_v0 v0) = isCanonical v0

-- | @set_param@: states @value@ for @id@, rebuilding the list as a
-- canonical prefix that carries every other parameter's value in force.
-- The list must already be canonical over the ids the constructor reads
-- (CHECK), even where the rebuilt list would not need them.
setParam :: NetworkConfig -> ConfigParamID -> Word64 -> NetworkConfig
setParam (NetworkConfig_v0 v0) pid@(ConfigParamID idv) value =
  -- Force the CHECKs (canonical input, each arm's width) here, where the
  -- C++ performs them, rather than wherever the list is first read.
  current `seq` foldr seq () rebuilt `seq` NetworkConfig_v0 NetworkConfigV0 {params = rebuilt}
  where
    rebuilt =
      [ makeParam i (if i == pid then value else inForce current i)
      | n <- [0 .. highest]
      , let i = ConfigParamID n
      ]
    current = networkConfigParams v0
    -- int32_t arithmetic, as in C++: max(id, size - 1).
    highest = max idv (fromIntegral (length v0.params) - 1 :: Int32)

-- | @make_param@: a 'ConfigParam' stating @value@ for @id@. A uint32 arm
-- CHECKs rather than narrowing, which would state a value the caller never
-- asked for.
makeParam :: ConfigParamID -> Word64 -> ConfigParam
makeParam pid value = case pid of
  KConfigParamMaxSHA256Buffers -> ConfigParam_max_sha256_buffers $! toU32 value
  KConfigParamMaxCrossInvokeDepth -> ConfigParam_max_cross_invoke_depth $! toU32 value
  KConfigParamMaxNewContractsPerTx -> ConfigParam_max_new_contracts_per_tx $! toU32 value
  KConfigParamMaxBytestringLen -> ConfigParam_max_bytestring_len $! toU32 value
  KConfigParamMaxContractSizeBytes -> ConfigParam_max_contract_size_bytes $! toU32 value
  KConfigParamMaxSavepointDepth -> ConfigParam_max_savepoint_depth $! toU32 value
  KConfigParamMaxSHA256BufferBytes -> ConfigParam_max_sha256_buffer_bytes $! toU32 value
  KConfigParamMaxAuthTokenDepth -> ConfigParam_max_auth_token_depth $! toU32 value
  KConfigParamMaxRv32CodeBytes -> ConfigParam_max_rv32_code_bytes $! toU32 value
  KConfigParamMaxRv32MemoryBytes -> ConfigParam_max_rv32_memory_bytes $! toU32 value
  KConfigParamMaxAuthTokens -> ConfigParam_max_auth_tokens $! toU32 value
  KConfigParamMaxCalldataLen -> ConfigParam_max_calldata_len $! toU32 value
  KConfigParamMaxReturnBufferLen -> ConfigParam_max_return_buffer_len $! toU32 value
  KConfigParamMaxTxGas -> ConfigParam_max_tx_gas $! value
  _ -> unknownId pid

-- | @stated_defaults@: a canonical configuration stating every parameter
-- this build knows at its default.
statedDefaults :: NetworkConfig
statedDefaults =
  setParam
    (NetworkConfig_v0 (NetworkConfigV0 []))
    kHighestConfigParamID
    (inForce defaultNetworkConfigParams kHighestConfigParamID)

-- | @to_network_config@: a complete reading of the values execution uses,
-- including defaults.
toNetworkConfig :: NetworkConfigParams -> NetworkConfig
toNetworkConfig p =
  NetworkConfig_v0 NetworkConfigV0 {params = [makeParam i (inForce p i) | i <- allConfigParamIDs]}

-- | The highest parameter id this build knows. A configuration states its
-- ids contiguously from zero, so this bounds a scan across all of them.
kHighestConfigParamID :: ConfigParamID
kHighestConfigParamID = KConfigParamMaxTxGas

-- | Ids @0 .. kHighestConfigParamID@, in order.
allConfigParamIDs :: [ConfigParamID]
allConfigParamIDs = let ConfigParamID h = kHighestConfigParamID in map ConfigParamID [0 .. h]

-- | @config_param_name@: wire name for @id@ (--network-config, recorded
-- overrides, genesis JSON).
configParamName :: ConfigParamID -> String
configParamName = \case
  KConfigParamMaxSHA256Buffers -> "max_sha256_buffers"
  KConfigParamMaxCrossInvokeDepth -> "max_cross_invoke_depth"
  KConfigParamMaxNewContractsPerTx -> "max_new_contracts_per_tx"
  KConfigParamMaxBytestringLen -> "max_bytestring_len"
  KConfigParamMaxContractSizeBytes -> "max_contract_size_bytes"
  KConfigParamMaxSavepointDepth -> "max_savepoint_depth"
  KConfigParamMaxSHA256BufferBytes -> "max_sha256_buffer_bytes"
  KConfigParamMaxAuthTokenDepth -> "max_auth_token_depth"
  KConfigParamMaxRv32CodeBytes -> "max_rv32_code_bytes"
  KConfigParamMaxRv32MemoryBytes -> "max_rv32_memory_bytes"
  KConfigParamMaxAuthTokens -> "max_auth_tokens"
  KConfigParamMaxCalldataLen -> "max_calldata_len"
  KConfigParamMaxReturnBufferLen -> "max_return_buffer_len"
  KConfigParamMaxTxGas -> "max_tx_gas"
  pid -> unknownId pid

-- | @config_param_id@: the parameter @name@ names, scanning ids
-- @0 .. kHighestConfigParamID@.
configParamId :: String -> Maybe ConfigParamID
configParamId name = case filter ((== name) . configParamName) allConfigParamIDs of
  (pid : _) -> Just pid
  [] -> Nothing

-- | @config_param_max_value@: the largest value @id@'s arm can carry.
configParamMaxValue :: ConfigParamID -> Word64
configParamMaxValue = \case
  KConfigParamMaxTxGas -> maxBound
  pid
    | pid >= KConfigParamMaxSHA256Buffers && pid <= KConfigParamMaxReturnBufferLen ->
        fromIntegral (maxBound :: Word32)
    | otherwise -> unknownId pid

-- | @config_param_value@: the value @param@ states, read from the arm it
-- names.
configParamValue :: ConfigParam -> Word64
configParamValue = \case
  ConfigParam_max_sha256_buffers v -> fromIntegral v
  ConfigParam_max_cross_invoke_depth v -> fromIntegral v
  ConfigParam_max_new_contracts_per_tx v -> fromIntegral v
  ConfigParam_max_bytestring_len v -> fromIntegral v
  ConfigParam_max_contract_size_bytes v -> fromIntegral v
  ConfigParam_max_savepoint_depth v -> fromIntegral v
  ConfigParam_max_sha256_buffer_bytes v -> fromIntegral v
  ConfigParam_max_auth_token_depth v -> fromIntegral v
  ConfigParam_max_rv32_code_bytes v -> fromIntegral v
  ConfigParam_max_rv32_memory_bytes v -> fromIntegral v
  ConfigParam_max_auth_tokens v -> fromIntegral v
  ConfigParam_max_calldata_len v -> fromIntegral v
  ConfigParam_max_return_buffer_len v -> fromIntegral v
  ConfigParam_max_tx_gas v -> v

unknownId :: ConfigParamID -> a
unknownId (ConfigParamID v) = error ("Unrecoverable; unknown ConfigParamID: " ++ show v)

checkFailed :: String -> a
checkFailed msg = error ("Unrecoverable; " ++ msg)
