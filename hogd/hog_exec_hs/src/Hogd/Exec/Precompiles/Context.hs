-- | make_hog_exec_context (precompiles/context.cc).  Placeholder until the
-- native precompiles land: RV32 only.
module Hogd.Exec.Precompiles.Context (makeHogExecContext) where

import qualified Data.Map.Strict as M
import Hogd.Exec.Runtime.NativeRuntime (multiplexedContext)
import Hogd.Exec.Runtime.Rv32Runtime (rv32Context)
import Hogd.Exec.Runtime.Types (RuntimeContext)
import Hogd.Xdr.Contract (pattern KRv32)

makeHogExecContext :: RuntimeContext
makeHogExecContext = multiplexedContext (M.fromList [(KRv32, rv32Context)])
