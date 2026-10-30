-- | Placeholder until the precompiles land.
module Hogd.Exec.Precompiles.Context (makeHogExecContext) where

import qualified Data.Map.Strict as M
import Hogd.Exec.Runtime.NativeRuntime (multiplexedContext)
import Hogd.Exec.Runtime.Types (RuntimeContext)

makeHogExecContext :: RuntimeContext
makeHogExecContext = multiplexedContext M.empty
