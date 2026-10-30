-- | Whatever a runtime builds while verifying a contract and needs again to
-- instantiate it (runtime/prepared_contract.h).  For RV32, verifying is
-- translating; keeping the image lets a same-transaction deployment reuse it
-- (#3536).
module Hogd.Exec.Runtime.PreparedContract
  ( PreparedContract (..)
  , emptyPreparedContract
  , prepareStoredRv32
  ) where

import Hogd.Exec.Runtime.Rv32.Artifact (ArtifactError)
import Hogd.Exec.Runtime.Rv32.Program (ceilingLimits)
import Hogd.Exec.Runtime.Rv32.Rv32Contract (Rv32Contract, createContract)
import Hogd.Xdr.Contract (ContractBlob (..))

newtype PreparedContract = PreparedContract {pcRv32 :: Maybe Rv32Contract}

emptyPreparedContract :: PreparedContract
emptyPreparedContract = PreparedContract Nothing

-- | Translate an already-deployed contract.  Deployments keep running under
-- the VM ceilings even if the chain later lowers its admission limits.
prepareStoredRv32 :: ContractBlob -> Either ArtifactError Rv32Contract
prepareStoredRv32 blob = createContract ceilingLimits blob.data_
