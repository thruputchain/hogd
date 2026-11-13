-- | The VM-facing part of @hogd/hog_exec/runtime/rv32/rv32_contract.cc@:
-- translating an artifact into a prepared contract (@Rv32Contract::create@),
-- instantiating machines from it (@Rv32Contract::instantiate@ plus the
-- method-id seeding of @Rv32Runtime::invoke_internal@), and the wire reason
-- for a refused artifact (@to_contract_reject_code@).
--
-- The ecall loop itself (@Rv32Runtime::run_ecall_loop@) belongs to the
-- executor, which owns @syscall_entry@. It drives a 'Machine' as follows:
--
-- > m <- instantiate prepared methodId gasLimit
-- > r <- start m
-- > -- while r == StepEcall:
-- > --   Just args <- pendingEcall m      -- a0 = syscall number, a1..a7 args
-- > --   remaining <- gasRemaining m       -- ExecutionContext.gas_remaining
-- > --   (hostResult, gasConsumed) <- syscall_entry ... (readGuest/writeGuest)
-- > --   on success:  r <- resumeFromEcall m (continueWith code gasConsumed)
-- > --   on error:    _ <- consumeGas m gasConsumed; finish with the error
-- > -- on StepTrap: TrapOutOfGas -> OutOfGas, any other trap -> Deterministic
-- > -- (StepHalted never happens: the loop never passes a halt result)
-- > -- gas consumed = gasLimit - gasRemaining m
module Hogd.Exec.Runtime.Rv32.Rv32Contract
  ( Rv32Contract
  , createContract
  , prepareArtifact
  , contractPrepared
  , instantiateContract
  , toContractRejectCode
  , verifyRv32Blob
  ) where

import Data.ByteString (ByteString)
import Data.Word (Word64)
import Hogd.Exec.Runtime.Rv32.Artifact
import Hogd.Exec.Runtime.Rv32.Machine
import Hogd.Exec.Runtime.Rv32.Program (Rv32Limits)
import Hogd.Xdr.DbConflict (ContractRejectCode, pattern ContractRejectCodeSizeInvalid, pattern ContractRejectDigestMismatch, pattern ContractRejectGasScheduleMismatch, pattern ContractRejectInitialMemoryInvalid, pattern ContractRejectInvalidProgram, pattern ContractRejectMalformedArtifact, pattern ContractRejectMemoryTooLarge, pattern ContractRejectUnsupportedAbi, pattern ContractRejectUnsupportedFormat)

-- | A translated, ready-to-instantiate RV32 contract. Immutable and safe to
-- share between concurrent invocations (the executor's snapshot cache and
-- per-transaction translation cache hold these).
newtype Rv32Contract = Rv32Contract PreparedContract
  deriving (Show)

contractPrepared :: Rv32Contract -> PreparedContract
contractPrepared (Rv32Contract p) = p

-- | @Rv32Contract::create(blob, limits)@: parse under @limits@ (the chain's
-- configured bounds at attachment, 'Hogd.Exec.Runtime.Rv32.Program.ceilingLimits'
-- for an already-deployed contract) and translate. Names the first artifact
-- check that refused it; code that parses cleanly but does not translate --
-- an undecodable or misaligned instruction stream, code past the VM's code
-- ceiling, or a declared memory size past the VM's memory ceiling (only
-- reachable with limits above the ceilings) -- is 'ArtifactInvalidProgram'.
createContract :: Rv32Limits -> ByteString -> Either ArtifactError Rv32Contract
createContract limits blob = parseArtifact limits blob >>= prepareArtifact

-- | The translation half of 'createContract'
-- (@ThreadedMachinePreInstantiation::create@ over a parsed artifact).
prepareArtifact :: Artifact -> Either ArtifactError Rv32Contract
prepareArtifact a =
  case preInstantiate (artifactCode a) (artifactInitialMemory a) (artifactInitialMemoryStart a) (artifactMemoryBytes a) of
    Left _ -> Left ArtifactInvalidProgram
    Right p -> Right (Rv32Contract p)

-- | @Rv32Contract::instantiate(gas)@ + @seed_method_id(method_id)@.
instantiateContract :: Rv32Contract -> Word64 -> Word64 -> IO Machine
instantiateContract (Rv32Contract p) = instantiate p

-- | The wire reason for an artifact 'createContract' refused. Coarser than
-- 'ArtifactError': the wire names what an author can act on.
toContractRejectCode :: ArtifactError -> ContractRejectCode
toContractRejectCode = \case
  ArtifactTooSmall -> ContractRejectMalformedArtifact
  ArtifactBadMagic -> ContractRejectMalformedArtifact
  ArtifactBadHeaderSize -> ContractRejectMalformedArtifact
  ArtifactInvalidSectionLayout -> ContractRejectMalformedArtifact
  ArtifactUnsupportedFormatVersion -> ContractRejectUnsupportedFormat
  ArtifactUnsupportedVmProfile -> ContractRejectUnsupportedAbi
  ArtifactUnsupportedFlags -> ContractRejectUnsupportedAbi
  ArtifactUnsupportedEntryAbi -> ContractRejectUnsupportedAbi
  ArtifactUnsupportedSyscallAbi -> ContractRejectUnsupportedAbi
  ArtifactGasScheduleVersionMismatch -> ContractRejectGasScheduleMismatch
  ArtifactGasScheduleDigestMismatch -> ContractRejectGasScheduleMismatch
  ArtifactMemorySizeInvalid -> ContractRejectMemoryTooLarge
  ArtifactCodeSizeInvalid -> ContractRejectCodeSizeInvalid
  ArtifactInitialMemoryTooLarge -> ContractRejectInitialMemoryInvalid
  ArtifactCodeDigestMismatch -> ContractRejectDigestMismatch
  ArtifactInitialMemoryDigestMismatch -> ContractRejectDigestMismatch
  ArtifactInvalidProgram -> ContractRejectInvalidProgram

-- | The RV32 part of @Rv32Context::verify_contract@ for a blob already known
-- to be an RV32 contract: translate under the chain's limits and hand the
-- translation back for reuse, or the wire rejection. (The contract-type
-- check and the @InvokeErrorCode@ belong to the executor.)
verifyRv32Blob :: Rv32Limits -> ByteString -> Either ContractRejectCode Rv32Contract
verifyRv32Blob limits blob = either (Left . toContractRejectCode) Right (createContract limits blob)
