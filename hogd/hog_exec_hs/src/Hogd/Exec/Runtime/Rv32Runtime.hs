-- | Placeholder until the RV32 VM port lands.
module Hogd.Exec.Runtime.Rv32Runtime
  ( Rv32Contract
  , prepareStoredRv32
  ) where

import Hogd.Xdr

data Rv32Contract = Rv32Contract

prepareStoredRv32 :: ContractBlob -> Either () Rv32Contract
prepareStoredRv32 _ = Left ()
