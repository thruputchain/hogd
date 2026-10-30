-- | Whatever a runtime builds while verifying a contract and needs again to
-- instantiate it (runtime/prepared_contract.h).  For RV32, verifying is
-- translating; keeping the image lets a same-transaction deployment reuse it
-- (#3536).
module Hogd.Exec.Runtime.PreparedContract
  ( PreparedContract (..)
  , emptyPreparedContract
  ) where

import Hogd.Exec.Runtime.Rv32Runtime (Rv32Contract)

newtype PreparedContract = PreparedContract {pcRv32 :: Maybe Rv32Contract}

emptyPreparedContract :: PreparedContract
emptyPreparedContract = PreparedContract Nothing
