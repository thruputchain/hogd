-- | Test-only contracts written as native functions, and a runtime context
-- for "mock" contracts (ContractType kMockTest) so tests can deploy
-- contracts at run time, which native contracts cannot be.
--
-- The driver contracts report what they observed through their own return
-- buffer, as a record:
--
-- * @[0x00, frame_status] ++ child buffer@ (or @0xFF@ when the child left
--   none) after a child frame ran;
-- * @[0x01, syscall error code]@ when the invoke was rejected before a
--   frame ran;
-- * @[0xEE, host error code]@ on a host-level failure.
module Harness.NativeContracts
  ( -- * Drivers
    forwarder
  , forwarderCalldata
  , invokeRecord
  , permitDriver
  , mintAuthToken
    -- * Simple callees
  , echoContract
  , abortWithCalldata
  , faultContract
  , singleMethod
    -- * Records
  , Record (..)
  , parseRecord
    -- * Mock contracts
  , mockContext
  , mockContractXdr
  , withMockContext
  ) where

import qualified Data.ByteString as B
import Data.Int (Int32)
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Exec.Common.Status
import Hogd.Exec.Log (logError)
import Hogd.Exec.Runtime.ContractTrait (entrypointFromMethodId, sdkMethodId)
import Hogd.Exec.Runtime.ContractTraitClient (nscSyscall, zeroInvokeOutputs)
import Hogd.Exec.Runtime.Item.StorageSnapshotCache (DeployedContract (..))
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.PreparedContract (emptyPreparedContract)
import Hogd.Exec.Runtime.SyscallHandler (getCalldata, getReturnBuffer, resolveContract, setReturnBuffer)
import Hogd.Exec.Runtime.Syscalls
import Hogd.Exec.Runtime.Types
import Hogd.Xdr
import X23.Xdr (xdrEncodeStrict)

-- | The forwarder's calldata: target, method id (big-endian), then the
-- inner calldata verbatim.
forwarderCalldata :: ContractAddress -> MethodID -> Calldata -> Calldata
forwarderCalldata target method inner = target <> entrypointFromMethodId method <> inner

-- | Invoke @method@ on @target@ (unbounded gas, no flags) with the given
-- auth-token ids and return the outcome record.
invokeRecord :: NativeSyscallContext -> ContractAddress -> MethodID -> Calldata -> [Word32] -> IO B.ByteString
invokeRecord ctx target method inner tokens = do
  let h = nscHandler ctx
      tokenBytes = B.concat (map le32 tokens)
  (r, outs) <- nscSyscall ctx KInvoke zeroInvokeOutputs $ \g -> sysInvoke h g target inner tokenBytes (method, maxBound, 0)
  case r of
    Left (HostFnErrorCode e) -> pure (B.pack [0xEE, fromIntegral e])
    Right c@(SyscallErrorCode code)
      | c /= KSyscallErrorCodeNone -> pure (B.pack [0x01, fromIntegral code])
      | otherwise -> do
          let InvocationFrameStatusCode st = ioFrameStatus outs
          rb <- getReturnBuffer h
          pure . (B.pack [0x00, fromIntegral st] <>) $ case rb of
            Right (Right buf) -> buf
            _ -> B.singleton 0xFF

-- | Method 0: forward per 'forwarderCalldata' and return the record.
forwarder :: NativeContract
forwarder = singleMethod $ \ctx -> do
  let h = nscHandler ctx
  cd <- getCalldata h
  let target = B.take 32 cd
      method = sdkMethodId (B.take 8 (B.drop 32 cd))
  rec <- invokeRecord ctx target method (B.drop 40 cd) []
  setReturnBuffer h rec False []
  pure (Right ())

-- | Mint an auth token whose root node names @consumer@ and carries
-- @payload@; the token id.
mintAuthToken :: NativeSyscallContext -> ContractAddress -> B.ByteString -> IO (Either String Word32)
mintAuthToken ctx consumer payload = do
  let node = AuthTokenNode {consumer_address = consumer, payload = payload, child_nodes = []}
  (r, tid) <- nscSyscall ctx KMintAuthToken 0 $ \_ -> sysMintAuthToken (nscHandler ctx) 1000 (xdrEncodeStrict node)
  pure $ case r of
    Right KSyscallErrorCodeNone -> Right tid
    other -> Left ("MintAuthToken failed: " ++ show other)

-- | Method 0: mint a grant token for @token@ carrying @grant@, then invoke
-- @method@ on @token@ with @calldata@, forwarding the token's handle
-- (@forward = False@ mints but forwards nothing).  Returns the record.
permitDriver :: ContractAddress -> B.ByteString -> MethodID -> Calldata -> Bool -> NativeContract
permitDriver token grant method calldata forward = singleMethod $ \ctx -> do
  minted <- mintAuthToken ctx token grant
  case minted of
    Left err -> do
      logError err
      pure (Left Deterministic)
    Right tid -> do
      rec <- invokeRecord ctx token method calldata (if forward then [tid] else [])
      setReturnBuffer (nscHandler ctx) rec False []
      pure (Right ())

-- | Method 0: return the calldata.
echoContract :: NativeContract
echoContract = singleMethod $ \ctx -> do
  cd <- getCalldata (nscHandler ctx)
  setReturnBuffer (nscHandler ctx) cd False []
  pure (Right ())

-- | Method 0: abort with the calldata as the abort buffer.
abortWithCalldata :: NativeContract
abortWithCalldata = singleMethod $ \ctx -> do
  cd <- getCalldata (nscHandler ctx)
  setReturnBuffer (nscHandler ctx) cd True []
  pure (Right ())

-- | Method 0: fault.
faultContract :: NativeContract
faultContract = singleMethod $ \_ -> pure (Left Deterministic)

singleMethod :: NativeFunction -> NativeContract
singleMethod f = mkNativeContract [(0, f)]

-- | A parsed driver record.
data Record
  = RecFrame InvocationFrameStatusCode B.ByteString
  | RecRejected SyscallErrorCode
  | RecHostError HostFnErrorCode
  | RecMalformed B.ByteString
  deriving (Eq, Show)

parseRecord :: B.ByteString -> Record
parseRecord b = case B.unpack (B.take 2 b) of
  [0x00, st] -> RecFrame (InvocationFrameStatusCode (fromIntegral st)) (B.drop 2 b)
  [0x01, c] -> RecRejected (SyscallErrorCode (fromIntegral c))
  [0xEE, c] -> RecHostError (HostFnErrorCode (fromIntegral c))
  _ -> RecMalformed b

-- | A mock contract record.
mockContractXdr :: Int32 -> Contract
mockContractXdr = Contract_mock_contract_id

-- | A runtime context for kMockTest contracts: each mock id runs a native
-- contract.
mockContext :: M.Map Int32 NativeContract -> RuntimeContext
mockContext contracts =
  RuntimeContext
    { rcVerifyContract = \_ _ -> pure (Right emptyPreparedContract)
    , rcMakeNewRuntime = \dep h -> do
        r <- resolveContract h (dcHash dep).hash
        case r of
          Left e -> pure (Left e)
          Right (Just (Contract_mock_contract_id i)) -> case M.lookup i contracts of
            Just nc -> pure (Right (Just (Runtime (\m g -> nativeContractInvoke nc m h g))))
            Nothing -> pure (Right Nothing)
          Right _ -> pure (Left SysUnrecoverable)
    }

-- | A multiplexed context of a native context and a mock context.
withMockContext :: RuntimeContext -> M.Map Int32 NativeContract -> RuntimeContext
withMockContext native mocks = multiplexedContext (M.fromList [(KNative, native), (KMockTest, mockContext mocks)])
