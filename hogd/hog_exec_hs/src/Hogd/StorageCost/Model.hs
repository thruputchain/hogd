{-# LANGUAGE NoFieldSelectors #-}

-- | Port of @hogd/storage_cost/storage_cost_model.{h,cc}@: the storage cost
-- model, in native-token units.
--
-- Every cost is an @int128_t@ ('Int128'). The C++ computes them as
-- @uint64_t + int128_t{n} * uint64_t@ (or @* uint32_t@): both operands are
-- converted to @__int128@ and the products and sums wrap modulo 2^128, which
-- 'Int128' arithmetic reproduces. (No reachable input overflows; the
-- point of the 128-bit arithmetic is that @n * per_byte@ does not truncate
-- at 64 bits.)
module Hogd.StorageCost.Model
  ( StorageCostModel (..)
  , storageCostModelFromRaw
  , storageCostModelFromXdr
  , storageCostModelToXdr
  , defaultCostModel
  , getBytestringCost
  , getNnintCost
  , getRetrocausalQueueCost
  , getReplayCacheCost
  , getDeploymentCost
  , getAuthDataCost
  , getBalanceCost
  ) where

import Data.Int (Int64)
import Data.Word (Word32, Word64)
import Hogd.Base.Types (Int128, SizeT)
import Hogd.Xdr.Contract (ContractType (..), DeployedContractHash (..), pattern KMockTest, pattern KNative, pattern KRv32)
import Hogd.Xdr.StorageCost (StorageCostModelXdr (..))
import Hogd.Xdr.Types (ExtensionPoint (..))

-- | @struct StorageCostModel@. Field names follow the C++ members.
data StorageCostModel = StorageCostModel
  { kBaseItemCost :: !Word64
  , kBytestringCostPerByte :: !Word32
  , kBaseContractDeploymentCost :: !Word64
  , kContractDeploymentCostPerByte :: !Word64
  , kAuthPerKeyCost :: !Word64
  , kRCQCostPerEntry :: !Word64
  , kReplayCacheCostPerEntry :: !Word64
  , kBalanceCost :: !Word64
  }
  deriving (Eq, Show)

-- | The raw-parameter constructor (only tests use it in C++): the
-- deployment base is the item base, and deployment-per-byte and
-- auth-per-key are both 1.
storageCostModelFromRaw :: Word64 -> Word32 -> Word64 -> Word64 -> Word64 -> StorageCostModel
storageCostModelFromRaw baseItemCost bytestringCostPerByte rcqCostPerEntry replayCacheCostPerEntry balanceCost =
  StorageCostModel
    { kBaseItemCost = baseItemCost
    , kBytestringCostPerByte = bytestringCostPerByte
    , kBaseContractDeploymentCost = baseItemCost
    , kContractDeploymentCostPerByte = 1
    , kAuthPerKeyCost = 1
    , kRCQCostPerEntry = rcqCostPerEntry
    , kReplayCacheCostPerEntry = replayCacheCostPerEntry
    , kBalanceCost = balanceCost
    }

-- | @StorageCostModel(StorageCostModelXdr const &)@.
storageCostModelFromXdr :: StorageCostModelXdr -> StorageCostModel
storageCostModelFromXdr m =
  StorageCostModel
    { kBaseItemCost = m.base_data_item_cost
    , kBytestringCostPerByte = m.bytestring_cost_per_byte
    , kBaseContractDeploymentCost = m.base_contract_deployment_cost
    , kContractDeploymentCostPerByte = m.contract_deployment_cost_per_byte
    , kAuthPerKeyCost = m.auth_per_key_cost
    , kRCQCostPerEntry = m.rcq_cost_per_entry
    , kReplayCacheCostPerEntry = m.replay_cache_cost_per_entry
    , kBalanceCost = m.balance_cost
    }

-- | @to_xdr@ (the extension point is always arm 0).
storageCostModelToXdr :: StorageCostModel -> StorageCostModelXdr
storageCostModelToXdr m =
  StorageCostModelXdr
    { ext = ExtensionPoint_0
    , base_data_item_cost = m.kBaseItemCost
    , bytestring_cost_per_byte = m.kBytestringCostPerByte
    , base_contract_deployment_cost = m.kBaseContractDeploymentCost
    , contract_deployment_cost_per_byte = m.kContractDeploymentCostPerByte
    , auth_per_key_cost = m.kAuthPerKeyCost
    , rcq_cost_per_entry = m.kRCQCostPerEntry
    , replay_cache_cost_per_entry = m.kReplayCacheCostPerEntry
    , balance_cost = m.kBalanceCost
    }

-- | @default_cost_model()@.
defaultCostModel :: StorageCostModel
defaultCostModel =
  storageCostModelFromXdr
    StorageCostModelXdr
      { ext = ExtensionPoint_0
      , base_data_item_cost = 12000
      , bytestring_cost_per_byte = 10
      , base_contract_deployment_cost = 12000
      , contract_deployment_cost_per_byte = 10
      , auth_per_key_cost = 1111
      , rcq_cost_per_entry = 8
      , replay_cache_cost_per_entry = 7
      , balance_cost = 1234
      }

i128 :: Integral a => a -> Int128
i128 = fromIntegral

-- | @get_bytestring_cost(size_t)@: base + len * per-byte.
getBytestringCost :: StorageCostModel -> SizeT -> Int128
getBytestringCost m len = i128 m.kBaseItemCost + (i128 len * i128 m.kBytestringCostPerByte)

-- | @get_nnint_cost()@: the item base.
getNnintCost :: StorageCostModel -> Int128
getNnintCost m = i128 m.kBaseItemCost

-- | @get_retrocausal_queue_cost(int64_t)@: zero for an empty queue, else
-- base + size * per-entry. The size is signed; a negative one is priced as
-- written.
getRetrocausalQueueCost :: StorageCostModel -> Int64 -> Int128
getRetrocausalQueueCost m activeQueueSize
  | activeQueueSize == 0 = 0
  | otherwise = i128 m.kBaseItemCost + (i128 activeQueueSize * i128 m.kRCQCostPerEntry)

-- | @get_replay_cache_cost(size_t)@: base + size * per-entry.
getReplayCacheCost :: StorageCostModel -> SizeT -> Int128
getReplayCacheCost m size = i128 m.kBaseItemCost + (i128 size * i128 m.kReplayCacheCostPerEntry)

-- | @get_deployment_cost@: native and mock contracts carry no deployable
-- blob, so deploy free; an RV32 contract pays base + size * per-byte. Any
-- other contract type is unrecoverable (C++ @panic@).
getDeploymentCost :: StorageCostModel -> DeployedContractHash -> Int128
getDeploymentCost m h = case h.contract_type of
  KNative -> 0
  KMockTest -> 0
  KRv32 ->
    i128 m.kBaseContractDeploymentCost
      + (i128 h.contract_size * i128 m.kContractDeploymentCostPerByte)
  ContractType t -> error ("Unrecoverable; Invalid contract type: " ++ show t)

-- | @get_auth_data_cost(size_t)@: base + keys * per-key.
getAuthDataCost :: StorageCostModel -> SizeT -> Int128
getAuthDataCost m numKeys = i128 m.kBaseItemCost + (i128 numKeys * i128 m.kAuthPerKeyCost)

-- | @get_balance_cost()@.
getBalanceCost :: StorageCostModel -> Int128
getBalanceCost m = i128 m.kBalanceCost
