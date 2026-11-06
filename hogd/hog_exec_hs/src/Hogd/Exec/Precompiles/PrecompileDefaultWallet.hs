-- | The native default wallet (precompiles/precompile_default_wallet.{h,cc}).
--
-- Native mirror of the default_wallet reference contract
-- (built_contracts/integration_test_contracts/default_wallet), with
-- byte-for-byte observable parity with its RV32 build.  Registered in
-- 'Hogd.Exec.Precompiles.Context' as kNativeContractDefaultWallet; reachable
-- only through root-resolution fallback for an address with no deployment.
module Hogd.Exec.Precompiles.PrecompileDefaultWallet
  ( makeNativeDefaultWalletContract
  , defaultWalletMethods
  , kMaxCalls
  , kMaxReturnDataBytes
  ) where

import qualified Data.ByteString as B
import Data.Word
import Hogd.BuiltContracts.Codec
import Hogd.BuiltContracts.DefaultWallet
import Hogd.BuiltContracts.ItcErc20 (Erc20Exn, erc20Trait)
import Hogd.Crypto.DomainSeparators (pubkeyAuthAddress)
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Log
import Hogd.Exec.Precompiles.NativeContractBuilder
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.ContractTraitClient
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallEntry (hostOutSpan)
import Hogd.Exec.Runtime.SyscallHandler (getCurrentAddress)
import Hogd.Exec.Runtime.Syscalls
import Hogd.Exec.Runtime.Types (MethodID)
import Hogd.Genesis.Constants (kNativeTokenContractAddress)
import Hogd.Xdr
import X23.Xdr (cxsGetWord64, cxsSize, deserializationRecursionLimit, runGet)

-- | Reference constants (implementation.rs).
kMaxCalls :: Int
kMaxCalls = 16

-- | implementation.rs compiles its own return-data limit in, so this follows
-- that contract rather than the chain's max return buffer length.
kMaxReturnDataBytes :: Int
kMaxReturnDataBytes = 1024

type WalletResult a = Result WalletExn a

walletErr :: WalletError -> WalletResult a
walletErr = Left . ErrException . WalletExn

zeroAddress :: ContractAddress
zeroAddress = B.replicate 32 0

-- | @require_root_invocation()@: GetMsgSender must fail with exactly
-- IsRootTransaction.
isRootInvocation :: NativeSyscallContext -> IO (InvokeStatus Bool)
isRootInvocation ctx = do
  (r, _) <- nscSyscall ctx KGetMsgSender zeroAddress $ \_ -> sysGetMsgSender (nscHandler ctx)
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right code -> pure (Right (code == KSyscallErrorCodeIsRootTransaction))

-- | @require_pubkey_signature_auth()@: the auth claim must be a pubkey
-- signature claim whose derived address is the current address.  Any
-- syscall error code, other claim type or mismatch is False.
hasValidPubkeyAuth :: NativeSyscallContext -> IO (InvokeStatus Bool)
hasValidPubkeyAuth ctx = do
  (r, ffiClaim) <- nscSyscall ctx KGetAuthClaim (B.replicate 48 0) $ \_ -> sysGetAuthClaim (nscHandler ctx)
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> pure (Right False)
      -- generated::AuthClaim: int32 tag; AuthClaim_kPubkeySignature = 1.
      | fromLE32 (B.take 4 ffiClaim) /= 1 -> pure (Right False)
      | otherwise -> do
          let preimage =
                PubkeyAccountPreimage
                  { pubkey_hash = B.take 32 (B.drop 8 ffiClaim)
                  , nonce = fromLE64 (B.take 8 (B.drop 40 ffiClaim))
                  }
          case pubkeyAuthAddress preimage of
            Nothing -> pure (Right False)
            Just derived -> do
              current <- getCurrentAddress (nscHandler ctx)
              pure (Right (derived == current))

-- | The shared auth preamble: Nothing when auth passed, otherwise the
-- WalletError to revert with.
checkRootPubkeyAuth :: NativeSyscallContext -> IO (InvokeStatus (Maybe WalletError))
checkRootPubkeyAuth ctx = do
  root <- isRootInvocation ctx
  case root of
    Left e -> pure (Left e)
    Right isRoot -> do
      -- Root-only reachability is enforced by resolution: a nested wallet
      -- frame is handler-level invariant breakage.
      checkM isRoot "default wallet invoked below the root frame"
      auth <- hasValidPubkeyAuth ctx
      pure $ case auth of
        Left e -> Left e
        Right False -> Right (Just InvalidAuthClaim)
        Right True -> Right Nothing

-- | @invoke_child@: one ordinary call, with the SDK's handling of the
-- child's outcome.  The ok value is the child's return buffer; an abort is
-- forwarded verbatim once its selector parses.
invokeChild :: NativeSyscallContext -> ContractAddress -> ContractEntrypointID -> Word64 -> B.ByteString -> IO (InvokeStatus (WalletResult B.ByteString))
invokeChild ctx target entrypoint gasLimit calldata = do
  let h = nscHandler ctx
      InvokeFlag noFlags = InvokeFlag_None
  (inv, outputs) <-
    nscSyscall ctx KInvoke zeroInvokeOutputs $ \g ->
      sysInvoke h g target calldata B.empty (sdkMethodId entrypoint, gasLimit, fromIntegral noFlags)
  case inv of
    Left e -> Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> pure (Right (walletErr CallFaulted))
      | ioFrameStatus outputs /= InvocationFrameStatusCode_Return && ioFrameStatus outputs /= Abort ->
          pure (Right (walletErr CallFaulted))
      | otherwise -> do
          let len = fromIntegral (ioReturnBufferLen outputs)
          (out, readOut) <- hostOutSpan len
          (get, (written, _, _)) <- nscSyscall ctx KGetReturnedBuffer (0, 0, 0) $ \_ -> sysGetReturnedBuffer h out
          case get of
            Left e -> Left <$> hostFnErrorConversion e
            Right c
              | c /= KSyscallErrorCodeNone || fromIntegral written /= len -> pure (Right (walletErr CallFaulted))
              | otherwise -> do
                  buf <- readOut
                  if ioFrameStatus outputs == Abort
                    then case runGet deserializationRecursionLimit cxsGetWord64 buf of
                      Left _ -> pure (Right (walletErr CallAborted))
                      Right _ -> pure (Right (Left (ErrRawAbort buf)))
                    else pure (Right (Right buf))

deployContract' :: NativeSyscallContext -> SHA256Hash -> SHA256Hash -> IO (InvokeStatus (WalletResult ContractAddress))
deployContract' ctx nonce contractHash = do
  (r, deployed) <- nscSyscall ctx KDeployContract zeroAddress $ \_ -> sysDeployContract (nscHandler ctx) nonce contractHash
  case r of
    Left e -> Left <$> hostFnErrorConversion e
    Right code
      | code /= KSyscallErrorCodeNone -> pure (Right (Left (ErrLibrary (syscallException KDeployContract code))))
      | otherwise -> pure (Right (Right deployed))

executeImpl :: NativeSyscallContext -> [WalletCall] -> IO (InvokeStatus (WalletResult [B.ByteString]))
executeImpl ctx calls = do
  authErr <- checkRootPubkeyAuth ctx
  case authErr of
    Left e -> pure (Left e)
    Right (Just err) -> pure (Right (walletErr err))
    Right Nothing
      | null calls -> pure (Right (walletErr EmptyCallList))
      | length calls > kMaxCalls -> pure (Right (walletErr TooManyCalls))
      | otherwise -> go calls [] (cxsSize (fromIntegral (length calls) :: Word32))
  where
    -- Running CXS size of the eventual Vec<WalletBytes>: the outer length
    -- prefix, then each item's prefix and bytes as it lands.
    go [] acc _ = pure (Right (Right (reverse acc)))
    go (call : rest) acc encodedLen = do
      child <- invokeChild ctx (wcTarget call) (wcEntrypoint call) (wcGasLimit call) (wcCalldata call)
      case child of
        Left e -> pure (Left e)
        Right (Left err) -> pure (Right (Left err))
        Right (Right buf) -> do
          let encodedLen' = encodedLen + cxsSize (fromIntegral (B.length buf) :: Word32) + B.length buf
          if encodedLen' > kMaxReturnDataBytes
            then pure (Right (walletErr ReturnDataTooLarge))
            else go rest (buf : acc) encodedLen'

deployImpl :: NativeSyscallContext -> (SHA256Hash, SHA256Hash, Integer, Maybe WalletInitCall) -> IO (InvokeStatus (WalletResult ContractAddress))
deployImpl ctx (nonce, contractHash, amount, initializer) = do
  authErr <- checkRootPubkeyAuth ctx
  case authErr of
    Left e -> pure (Left e)
    Right (Just err) -> pure (Right (walletErr err))
    Right Nothing -> do
      funded <- if amount /= 0 then fund else pure (Right Nothing)
      case funded of
        Left e -> pure (Left e)
        Right (Just failure) -> pure (Right (Left failure))
        Right Nothing -> do
          deployed <- deployContract' ctx nonce contractHash
          case deployed of
            Right (Right addr) -> case initializer of
              Nothing -> pure deployed
              Just ini -> do
                child <- invokeChild ctx addr (wicEntrypoint ini) (wicGasLimit ini) (wicCalldata ini)
                pure $ case child of
                  Left e -> Left e
                  Right (Left err) -> Right (Left err)
                  Right (Right _) -> deployed
            _ -> pure deployed
  where
    -- Transfer the requested native tokens to the deployment address through
    -- the ordinary token contract, preserving its validation, caller identity
    -- and events.
    fund = do
      (r, target) <-
        nscSyscall ctx KGetDeployAddress zeroAddress $ \_ ->
          sysGetDeployAddress (nscHandler ctx) Nothing nonce contractHash
      case r of
        Left e -> Left <$> hostFnErrorConversion e
        Right code
          | code /= KSyscallErrorCodeNone ->
              pure (Right (Just (ErrLibrary (syscallException KGetDeployAddress code))))
          | otherwise -> do
              let InvokeFlag noFlags = InvokeFlag_None
                  calldata = encodeC Cxs (pairC addressC i128C) (target, amount)
              transfer <-
                invokeOptResult ctx kNativeTokenContractAddress erc20Trait "transfer" (fromIntegral noFlags) calldata (decodeC Cxs unitC) ::
                  IO (HostFnStatus (SyscallStatus (Maybe (Result Erc20Exn ()))))
              case transfer of
                Left e -> Left <$> hostFnErrorConversion e
                Right (Left _) -> pure (Right (Just (ErrException (WalletExn CallFaulted))))
                Right (Right Nothing) -> pure (Right (Just (ErrException (WalletExn CallFaulted))))
                Right (Right (Just (Left err))) -> pure (Right (Just (ErrRawAbort (serializeContractError err))))
                Right (Right (Just (Right ()))) -> pure (Right Nothing)

-- | The method table of 'makeNativeDefaultWalletContract'.
defaultWalletMethods :: [(MethodID, NativeFunction)]
defaultWalletMethods =
  [ traitMethod defaultWalletTrait "execute" (argSpec walletCallsC) $ \ctx calls ->
      fmap (returnResult Cxs (codecPair walletReturnsC)) <$> executeImpl ctx calls
  , traitMethod defaultWalletTrait "deploy" (argSpec deployArgsC) $ \ctx args ->
      fmap (returnResult Cxs (codecPair addressC)) <$> deployImpl ctx args
  ]

-- | @make_native_default_wallet_contract@.
makeNativeDefaultWalletContract :: NativeContract
makeNativeDefaultWalletContract
  | checkMethods defaultWalletTrait defaultWalletMethods = buildNativeContract defaultWalletMethods
  | otherwise = error "default wallet is missing an entrypoint"
