-- | Port of @hogd/hog_exec/common/limits.h@: hog_exec-enforced bounds on
-- contract-controlled inputs, and the ceilings genesis load checks the
-- chain's configured parameters against.
--
-- The genesis-load and recovered-tip checks themselves live in hog_db
-- (@HogDbSchedRpc::dbsched_load_genesis_configuration@ in
-- hogd/hog_db/rpc_from_sched.cc, @StartupValidation::tip_is_servable@ in
-- hogd/hog_db/startup_validation.cc); the parameter rules they apply are
-- reproduced here as 'checkGenesisConfigParams' and
-- 'checkRecoveredTipConfigParams', so every bound the executor relies on
-- is stated in one place.
module Hogd.Exec.Common.Limits
  ( kMaxBytesTransaction
  , kSavepointDepthCeiling
  , kExecToDbMessageCap
  , kContractSizeCeilingBytes
  , kSerializedContractEnvelopeBytes
  , serializedContractBound
  , kMaxSerializedContractBytes
    -- * RV32 VM ceilings (hogd/hog_exec/runtime/rv32/program.h)
  , kRv32MemoryCeilingBytes
  , kRv32CodeCeilingBytes
  , kRv32DefaultMaxMemoryBytes
  , kRv32DefaultMaxCodeBytes
    -- * Configuration checks
  , kGenesisBatchOverhead
  , checkGenesisNetworkConfig
  , checkGenesisConfigParams
  , checkRecoveredTipConfigParams
  ) where

import Data.Word (Word32)
import Hogd.Base.NetworkConfigParams
import Hogd.Base.Types (SizeT, kiB)
import Hogd.Xdr

-- | Cap on the Transaction body, what TransactionHash commits to (#3844).
-- Below @kMaxBytesSignedTransaction@ (2'000'000), as limits.h
-- static_asserts.
kMaxBytesTransaction :: SizeT
kMaxBytesTransaction = 1900000

-- | Ceiling on @kConfigParamMaxSavepointDepth@, checked at genesis load:
-- each open savepoint is a nested VM on one executor thread's stack, and
-- this is the depth those stacks are sized for.
kSavepointDepthCeiling :: Word32
kSavepointDepthCeiling = kMaxSavepointDepth

-- | The budget the executor holds itself to on the HogDb link (pinned to
-- @kBulkRpcMaxMessageSize@). Raising it changes which transactions abort.
kExecToDbMessageCap :: SizeT
kExecToDbMessageCap = 0x1000000

-- | Ceiling on @kConfigParamMaxContractSizeBytes@, checked at genesis load.
-- A transaction body carries its attachments, so nothing above
-- 'kMaxBytesTransaction' could reach the executor.
kContractSizeCeilingBytes :: SizeT
kContractSizeCeilingBytes = kMaxBytesTransaction

-- | Bytes the XDR envelope adds to a blob in a serialized Contract row:
-- union tag 4 + opaque length 4 + padding <= 3, with slack.
kSerializedContractEnvelopeBytes :: SizeT
kSerializedContractEnvelopeBytes = 16

-- | @serialized_contract_bound@: bound on the serialized Contract row
-- holding a blob of @blob_bytes@ (size_t arithmetic, wrapping).
serializedContractBound :: SizeT -> SizeT
serializedContractBound blobBytes = blobBytes + kSerializedContractEnvelopeBytes

-- | Bound on the serialized Contract row hog_db accepts in
-- @InvokeTX::request_new_contract()@; exceeding it is executor misbehavior.
-- limits.h static_asserts that this plus 1 KiB fits 'kExecToDbMessageCap'.
kMaxSerializedContractBytes :: SizeT
kMaxSerializedContractBytes = serializedContractBound kContractSizeCeilingBytes

-- | @rv32::kMemoryCeilingBytes@: the most guest memory the VM provisions.
kRv32MemoryCeilingBytes :: Word32
kRv32MemoryCeilingBytes = 16 * 1024 * 1024

-- | @rv32::kCodeCeilingBytes@: the most code the VM translates.
kRv32CodeCeilingBytes :: Word32
kRv32CodeCeilingBytes = 4 * 1024 * 1024

-- | @rv32::kDefaultMaxMemoryBytes@ (restates the protocol default
-- @kDefaultMaxRv32MemoryBytes@; rv32_contract.cc pins them equal).
kRv32DefaultMaxMemoryBytes :: Word32
kRv32DefaultMaxMemoryBytes = 64 * 1024

-- | @rv32::kDefaultMaxCodeBytes@ (restates @kDefaultMaxRv32CodeBytes@).
kRv32DefaultMaxCodeBytes :: Word32
kRv32DefaultMaxCodeBytes = 256 * 1024

-- | @kBatchOverhead@ in @dbsched_load_genesis_configuration@.
kGenesisBatchOverhead :: SizeT
kGenesisBatchOverhead = kiB 1

-- | The network-config checks @dbsched_load_genesis_configuration@ applies
-- to an untrusted genesis, in order: canonical form (before hashing), then
-- 'checkGenesisConfigParams'. The 'Left' carries the C++ error text. (The
-- height / network hash / config hash check between the two needs the
-- whole genesis and is not reproduced.)
checkGenesisNetworkConfig :: NetworkConfig -> Either String NetworkConfigParams
checkGenesisNetworkConfig config
  | not (configHashable config) =
      Left
        "genesis network config parameters are not the gapless prefix 0, 1, \
        \2, ... of the parameter ids"
  | otherwise =
      let params = networkConfigParamsOf config
       in params <$ checkGenesisConfigParams params

-- | The ceiling rules @dbsched_load_genesis_configuration@ applies to the
-- parameters a genesis states (hogd/hog_db/rpc_from_sched.cc), in the
-- same order and with the same messages.
checkGenesisConfigParams :: NetworkConfigParams -> Either String ()
checkGenesisConfigParams p
  | stated_size > kContractSizeCeilingBytes =
      Left
        ( "genesis max_contract_size_bytes "
            ++ show stated_size
            ++ " exceeds the "
            ++ show kContractSizeCeilingBytes
            ++ " this build can store and a transaction can carry"
        )
  -- The commit batch a transaction can realize is the smaller of the stated
  -- count at the stated size and what one transaction body carries plus
  -- per-row slack; refuse the pair only if that cannot fit one message
  -- (#2190).
  | batchBytes > kExecToDbMessageCap =
      Left
        ( "genesis max_new_contracts_per_tx "
            ++ show count
            ++ " of max_contract_size_bytes "
            ++ show p.max_contract_size_bytes
            ++ " needs a "
            ++ show batchBytes
            ++ "-byte commit batch; this build sends at most "
            ++ show kExecToDbMessageCap
            ++ " in one message"
        )
  | p.max_rv32_code_bytes > kRv32CodeCeilingBytes =
      Left
        ( "genesis max_rv32_code_bytes "
            ++ show p.max_rv32_code_bytes
            ++ " exceeds the "
            ++ show kRv32CodeCeilingBytes
            ++ " this build's VM translates"
        )
  | p.max_rv32_memory_bytes > kRv32MemoryCeilingBytes =
      Left
        ( "genesis max_rv32_memory_bytes "
            ++ show p.max_rv32_memory_bytes
            ++ " exceeds the "
            ++ show kRv32MemoryCeilingBytes
            ++ " this build's VM provisions"
        )
  -- Every root commit pushes a savepoint, even without child TryInvokes.
  | p.max_savepoint_depth == 0 =
      Left "genesis max_savepoint_depth must be at least 1 for root commits"
  | p.max_savepoint_depth > kSavepointDepthCeiling =
      Left
        ( "genesis max_savepoint_depth "
            ++ show p.max_savepoint_depth
            ++ " exceeds the "
            ++ show kSavepointDepthCeiling
            ++ " this build's executor stacks sustain"
        )
  -- The ceiling bounds the wire type, so a genesis naming more would let a
  -- contract write a bytestring no peer could carry.
  | p.max_bytestring_len > (kBytestringCeilingLen :: Word32) =
      Left
        ( "genesis max_bytestring_len "
            ++ show p.max_bytestring_len
            ++ " exceeds the protocol ceiling of "
            ++ show (kBytestringCeilingLen :: Int)
        )
  | otherwise = Right ()
  where
    stated_size = fromIntegral p.max_contract_size_bytes :: SizeT
    count = fromIntegral p.max_new_contracts_per_tx :: SizeT
    statedBatch = count * serializedContractBound stated_size
    carriedBatch = kContractSizeCeilingBytes + (count * kSerializedContractEnvelopeBytes)
    batchBytes = min statedBatch carriedBatch + kGenesisBatchOverhead

-- | The genesis bounds @StartupValidation::tip_is_servable@ reapplies to a
-- recovered tip's configuration (#4336): the tip row is outside the state
-- hash, so a self-consistent rewrite could otherwise set a depth genesis
-- would reject.
checkRecoveredTipConfigParams :: NetworkConfigParams -> Either String ()
checkRecoveredTipConfigParams p
  | p.max_savepoint_depth == 0 =
      Left "recovered tip max_savepoint_depth must be at least 1 for root commits"
  | p.max_savepoint_depth > kSavepointDepthCeiling =
      Left
        ( "recovered tip max_savepoint_depth "
            ++ show p.max_savepoint_depth
            ++ " exceeds the "
            ++ show kSavepointDepthCeiling
            ++ " this build's executor stacks sustain"
        )
  | otherwise = Right ()
