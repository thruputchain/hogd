-- | The mutually dependent core types of the runtime: the per-transaction
-- 'SystemCallHandler', its invocation frames, and the 'Runtime' /
-- 'RuntimeContext' abstractions that run contract code against it
-- (runtime/runtime.h, runtime/invocation_frame.h, runtime/syscall_handler.h).
module Hogd.Exec.Runtime.Types
  ( MethodID
  , Runtime (..)
  , RuntimeContext (..)
  , SystemCallHandler (..)
  , InvocationFrame (..)
  , HashBuffers (..)
  , newHashBuffers
  ) where

import qualified Crypto.Hash as H
import Data.IORef
import qualified Data.Map.Strict as M
import Data.Sequence (Seq)
import qualified Data.Set as S
import Data.Word
import Hogd.Base.NetworkConfigParams (NetworkConfigParams)
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ExecutionMode
import Hogd.Exec.Runtime.EventServerProxy
import Hogd.Exec.Runtime.Item.DatabaseProxy
import Hogd.Exec.Runtime.Item.ItemView
import Hogd.Exec.Runtime.Item.StorageSnapshotCache
import Hogd.Exec.Runtime.PreparedContract
import Hogd.Xdr

-- | The 8-byte entrypoint id, read big-endian.
type MethodID = Word64

-- | One instantiated contract, ready to be invoked.  'rtInvokeInternal'
-- returns the outcome and the gas consumed, which must not exceed the limit.
newtype Runtime = Runtime
  { rtInvokeInternal :: MethodID -> Word64 -> IO (InvokeStatus (), Word64)
  }

-- | Shared, stateless per-engine context.
data RuntimeContext = RuntimeContext
  { rcVerifyContract
      :: Contract
      -> NetworkConfigParams
      -> IO (Either (InvokeErrorCode, Maybe ContractRejectCode) PreparedContract)
  -- ^ Left (Deterministic, Just reason) for an invalid contract; Left (other
  -- code, _) for a node fault.
  , rcMakeNewRuntime :: DeployedContract -> SystemCallHandler -> IO (SystemStatus (Maybe Runtime))
  -- ^ Nothing if there is no runnable contract (e.g. an undefined native id)
  }

-- | SHA256 streams keyed by contract-chosen index (components/hash_buffers.*).
data HashBuffers = HashBuffers
  { hbMax :: !Word32
  , hbStreams :: !(M.Map Word32 (H.Context H.SHA256))
  }

newHashBuffers :: Word32 -> HashBuffers
newHashBuffers m = HashBuffers m M.empty

-- | All data for one invocation within a transaction.
data InvocationFrame = InvocationFrame
  { ifAddress :: !ContractAddress
  , ifCalldata :: !Calldata
  , ifChildReturnOutput :: !(Maybe InvocationFrameOutput)
  -- ^ the last child's output; Nothing before any child call
  , ifChildReturnedTokens :: !(Word32, Word32)
  -- ^ (start, count): the ids the last child's returned tokens took here
  , ifToReturn :: !InvocationFrameOutput
  , ifReturnedTokenIds :: ![Word32]
  , ifFlags :: !InvokeFlagSet
  , ifEvents :: !(Seq TxEmittedEvent)
  , ifEventCount :: !Int
  , ifTryInvokeFrameCount :: !Word32
  -- ^ subtransactions this frame pushed for successful TryInvoke children
  , ifTotalTryInvokeDepth :: !Word32
  , ifModifiedItems :: !(M.Map AddressAndKey ItemView)
  , ifNewDeployments :: !(M.Map ContractAddress DeployedContractHash)
  , ifAuthTokens :: !(Seq (Maybe AuthToken))
  -- ^ indexed by token id; Nothing once a token left the frame
  , ifForwardedCount :: !Word32
  , ifChildForwarded :: ![Word32]
  , ifHashBuffers :: !HashBuffers
  }

-- | Everything that belongs to one running transaction.
data SystemCallHandler = SystemCallHandler
  { schRuntimeContext :: RuntimeContext
  , schDb :: PerTransactionDatabaseProxy
  , schEvents :: PerTransactionEventServerProxy
  , schFrames :: IORef [InvocationFrame]
  -- ^ the invocation stack, active frame first
  , schConfig :: NetworkConfigParams
  , schSourceTxHash :: !SHA256Hash
  , schAuthClaim :: !AuthClaim
  , schRootInvocation :: IORef (Maybe Invocation)
  , schUniquifier :: !(Maybe Uniquifier)
  , schRootInvokedAddress :: !(Maybe ContractAddress)
  , schMode :: ExecutionModeData
  , schContractsToDeploy :: [Contract]
  , schNewContracts :: IORef (M.Map SHA256Hash (Contract, PreparedContract))
  , schLastFrame :: IORef (Maybe InvocationFrame)
  , schRootResult :: IORef InvokeResult
  , schInitialValidationRejected :: IORef Bool
  , schFinalized :: IORef Bool
  , schLastFrameFinished :: IORef Bool
  , schCommittedOrAborted :: IORef Bool
  , schTwoPhaseCommitFailed :: IORef Bool
  , schSnapshot :: StorageSnapshotCache
  , schLoadedKeys :: IORef (S.Set AddressAndKey)
  -- ^ keys this transaction has loaded, rolled-back frames included
  }
