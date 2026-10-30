-- | The executor's runtime context (precompiles/context.{h,cc}): the native
-- precompiles multiplexed with the RV32 runtime.
module Hogd.Exec.Precompiles.Context
  ( makeHogExecPrecompilesContext
  , makeHogExecContextWith
  , makeHogExecContext
  , hogExecPrecompiles
  ) where

import qualified Data.Map.Strict as M
import Hogd.Exec.Runtime.Rv32Runtime (rv32Context)
import Hogd.Exec.Precompiles.PrecompileDefaultWallet (makeNativeDefaultWalletContract)
import Hogd.Exec.Precompiles.PrecompileNativeToken (makeNativeTokenContract)
import Hogd.Exec.Runtime.NativeRuntime (NativeContract, multiplexedContext, nativeContext)
import Hogd.Exec.Runtime.Types (RuntimeContext)
import Hogd.Xdr

-- | The precompiled native contracts, by id.  A top-level constant, so it is
-- built once and shared, as the C++ caches its context in a function-local
-- static.
hogExecPrecompiles :: M.Map HogexecPrecompiledNativeContracts NativeContract
hogExecPrecompiles =
  M.fromList
    [ (KNativeContractNativeToken, makeNativeTokenContract)
    , (KNativeContractDefaultWallet, makeNativeDefaultWalletContract)
    ]

-- | @make_hog_exec_precompiles_context@.
makeHogExecPrecompilesContext :: RuntimeContext
makeHogExecPrecompilesContext = nativeContext hogExecPrecompiles

-- | @make_hog_exec_context@ with the RV32 context supplied by the caller.
-- The RV32 runtime is ported separately; until it is plugged in here,
-- Passing Nothing leaves RV32 contracts without a runtime (they are rejected
-- as not deployable); 'makeHogExecContext' passes the RV32 context.
makeHogExecContextWith :: Maybe RuntimeContext -> RuntimeContext
makeHogExecContextWith rv32 =
  multiplexedContext . M.fromList $
    (KNative, makeHogExecPrecompilesContext) : maybe [] (\c -> [(KRv32, c)]) rv32

-- | @make_hog_exec_context@.
makeHogExecContext :: RuntimeContext
makeHogExecContext = makeHogExecContextWith (Just rv32Context)
