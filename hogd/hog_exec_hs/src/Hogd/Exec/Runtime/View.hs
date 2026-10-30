-- | Read-only view execution (runtime/view/view.{h,cc}).
module Hogd.Exec.Runtime.View
  ( runViewTransaction
  ) where

import qualified Data.ByteString as B
import Hogd.Base.NetworkConfigParams (NetworkConfigParams (..))
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ExecutionMode
import Hogd.Exec.Log
import Hogd.Exec.Runtime.EventServerProxy
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.StorageSnapshotCache
import Hogd.Exec.Runtime.SyscallHandler
import Hogd.Exec.Runtime.Types
import Hogd.Xdr
import X23.Xdr (xdrDecodeWith)

-- | Run one view query against the snapshot.  The cache's tip must already
-- be loaded or seeded.
runViewTransaction
  :: StorageSnapshotCache
  -> PerTransactionDatabaseProxy
  -> PerTransactionEventServerProxy
  -> TransactionViewQuery
  -> RuntimeContext
  -> NondeterministicProposalData
  -> IO ExpectedViewResult
runViewTransaction cache db ev query ctx ndet = do
  h <- newSystemCallHandlerTxless ctx cache db ev (ViewingModeCtor ndet) query.auth_claim (B.replicate 32 0)
  let invalid code = pure (ExpectedViewResult_error_code code)
      internal = pure (ExpectedViewResult_Abandoned InternalError)
  if not (null query.forwarded_tokens) && null query.parent_address
    then invalid KSyscallErrorCodeInvalidSyscallInput
    else do
      case query.parent_address of
        Just parent -> startFrame h parent mempty 0 []
        Nothing -> pure ()
      let depth = fromIntegral (configParams h).max_auth_token_depth
          decoded = mapM (xdrDecodeWith depth) query.forwarded_tokens :: Either String [AuthToken]
      case decoded of
        Left _ -> invalid KSyscallErrorCodeInvalidSyscallInput
        Right tokens -> do
          ids <- mapM (mintAuthToken h) tokens
          case sequence ids of
            Left _ -> internal
            Right tokenIds -> do
              admitted <- validateNewInvocation h query.invocation.invoked_address query.invoke_flags tokenIds
              case admitted of
                Left code -> do
                  logInfo ("view transaction failed with SyscallErrorCode::" ++ show code)
                  invalid code
                Right () -> do
                  (res, gas) <-
                    invoke
                      h
                      query.invocation.invoked_address
                      (sdkMethodId query.invocation.entrypoint)
                      query.invocation.gas_limit
                      query.invocation.calldata
                      query.invoke_flags
                      tokenIds
                  case res of
                    Left SystemErrorCode_ValidationFailure -> do
                      logError "view transaction failed with SystemErrorCode::ValidationFailure"
                      pure (ExpectedViewResult_Abandoned NondeterministicDataValidationFailure)
                    Left SystemErrorCode_Abandon -> pure (ExpectedViewResult_Abandoned Abandoned)
                    Left _ -> do
                      logError "view transaction failed with SystemErrorCode::Unrecoverable"
                      internal
                    Right (Left code) -> do
                      logInfo ("view transaction failed with SyscallErrorCode::" ++ show code)
                      invalid code
                    Right (Right _) -> do
                      out <- getMostRecentFrameOutput h
                      evs <- getMostRecentFrameEvents h
                      pure
                        ( ExpectedViewResult_result
                            TransactionViewResult {output = out, gas_consumed = gas, events = evs}
                        )
