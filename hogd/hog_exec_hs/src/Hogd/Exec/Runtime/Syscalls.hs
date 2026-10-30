-- | Every syscall body and the RV32 dispatch table
-- (runtime/syscall_*.cc, syscall_entry.cc, syscall_fallback.h).
--
-- Bodies are typed functions of their inputs ("inner" functions in C++);
-- 'rv32Syscalls' pairs each with the register/memory layout of its
-- arguments.  Native contracts call the bodies through 'nativeSyscall'.
module Hogd.Exec.Runtime.Syscalls
  ( -- * Dispatch
    syscallEntry
  , syscallMetaOf
    -- * Bodies (for native contracts)
  , InvokeOutputs (..)
  , sysSHA256
  , sysGetMsgSender
  , sysGetCalldataLen
  , sysGetCalldata
  , sysConsoleLog
  , sysInvoke
  , sysReturn
  , sysGetReturnedBuffer
  , sysExit
  , sysGetDeployAddress
  , sysDeployContract
  , sysGetSourceTransactionHash
  , sysVerifySignature
  , sysBytestringGetLen
  , sysBytestringGet
  , sysBytestringSet
  , sysNonnegativeIntGet
  , sysNonnegativeIntSetAdd
  , sysDeleteObject
  , sysGetCurrentAddress
  , sysRetrocausalQueueInsert
  , sysRetrocausalQueueConsume
  , sysGetTransactionStatistics
  , sysChangePublicKeyRegistration
  , sysLogEvent
  , sysGetAuthClaim
  , sysSHA256Update
  , sysSHA256Finalize
  , sysWriteAddressRequiredWeight
  , sysGetBlockHeight
  , sysRetrocausalQueuePeekLen
  , sysMintAuthToken
  , sysConsumeAuthToken
  , sysSHA256Copy
  , sysGetNetworkHash
  , sysGetBlockTimepoint
  , sysSetStorageFlags
  , sysGetStorageFlags
  , sysClearStorageFlags
    -- * Encodings
  , le32
  , le64
  , leI32
  , fromLE32
  , fromLE64
  , authClaimFfiBytes
  ) where

import Data.Bits
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.Int
import Data.List (nub, sort)
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Base.NetworkConfigParams (NetworkConfigParams (..))
import Hogd.Crypto (sha256)
import Hogd.Crypto.Ed25519 (ed25519Verify)
import Hogd.Crypto.MlDsa (mldsa65Verify)
import Hogd.Exec.Common.Int128
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Exec.Runtime.GasCostTable
import Hogd.Exec.Runtime.InvocationFrame (ConsumeAuthTokenResult (..))
import Hogd.Exec.Runtime.SyscallEntry
import Hogd.Exec.Runtime.SyscallHandler
import Hogd.Exec.Runtime.Types
import Hogd.Xdr.AddressAuth (PublicKeyConfig (..))
import Hogd.Xdr.AuthClaim
import Hogd.Xdr.AuthToken
import Hogd.Xdr.InvokeFlag (InvokeFlag (..), pattern TryInvoke, pattern ReadOnly)
import Hogd.Xdr.Syscall
import Hogd.Xdr.Transaction
import Hogd.Xdr.Types
import X23.Xdr (xdrDecodeWith)

------------------------------------------------------------------------------
-- Little-endian encodings (the guest ABI is little-endian)

le32 :: Word32 -> B.ByteString
le32 w = B.pack [fromIntegral (w `shiftR` (8 * i)) | i <- [0 .. 3]]

leI32 :: Int32 -> B.ByteString
leI32 = le32 . fromIntegral

le64 :: Word64 -> B.ByteString
le64 w = B.pack [fromIntegral (w `shiftR` (8 * i)) | i <- [0 .. 7]]

fromLE32 :: B.ByteString -> Word32
fromLE32 = B.foldr (\b acc -> (acc `shiftL` 8) .|. fromIntegral b) 0 . B.take 4

fromLE64 :: B.ByteString -> Word64
fromLE64 = B.foldr (\b acc -> (acc `shiftL` 8) .|. fromIntegral b) 0 . B.take 8

ok :: Word64 -> InnerResult
ok g = (Right KSyscallErrorCodeNone, g)

err :: SyscallErrorCode -> Word64 -> InnerResult
err e g = (Right e, g)

hostErr :: HostFnError -> Word64 -> InnerResult
hostErr e g = (Left e, g)

sysErr :: SystemError -> InnerResult
sysErr e = (Left (systemToHostFn e), 0)

------------------------------------------------------------------------------
-- Crypto

sysSHA256 :: SystemCallHandler -> Word64 -> B.ByteString -> IO (InnerResult, SHA256Hash)
sysSHA256 _ remaining input = do
  let required = gasSha256Hash (fromIntegral (B.length input))
  if remaining <= required
    then pure (hostErr HostFnErrorCode_OutOfGas remaining, zeroHash)
    else pure (ok required, sha256 input)

zeroHash :: B.ByteString
zeroHash = B.replicate 32 0

sysSHA256Update :: SystemCallHandler -> Word64 -> Word32 -> B.ByteString -> Bool -> IO (InnerResult, ())
sysSHA256Update h remaining idx input reset = do
  let required = gasSha256Update (fromIntegral (B.length input))
  if remaining <= required
    then pure (hostErr HostFnErrorCode_OutOfGas remaining, ())
    else do
      r <- sha256Update h idx input reset
      pure $ case r of
        Left e -> (err e kSHA256StreamCost, ())
        Right () -> (ok required, ())

sysSHA256Copy :: SystemCallHandler -> Word32 -> Word32 -> IO (InnerResult, ())
sysSHA256Copy h src dst = do
  r <- sha256Copy h src dst
  pure $ case r of
    Left e -> (err e kSHA256StreamCost, ())
    Right () -> (ok kSHA256StreamCost, ())

sysSHA256Finalize :: SystemCallHandler -> Word32 -> IO (InnerResult, SHA256Hash)
sysSHA256Finalize h idx = do
  r <- sha256Finalize h idx
  pure $ case r of
    Left e -> (err e kSHA256StreamCost, zeroHash)
    Right hsh -> (ok kSHA256HashBlockCost, hsh)

sysVerifySignature :: Word64 -> Int32 -> B.ByteString -> B.ByteString -> B.ByteString -> IO (InnerResult, ())
sysVerifySignature remaining scheme sig pk msg = pure (go, ())
  where
    go = case CryptographicScheme scheme of
      Ed25519
        | B.length pk /= 32 || B.length sig /= 64 -> err KSyscallErrorCodeInvalidSyscallInput 0
        | otherwise ->
            let gas = gasVerifySignature (fromIntegral (B.length msg))
             in if remaining <= gas
                  then hostErr HostFnErrorCode_OutOfGas remaining
                  else
                    if ed25519Verify pk sig msg
                      then ok gas
                      else err KSyscallErrorCodeInvalidSyscallInput gas
      MlDsa65
        | B.length pk /= 1952 || B.length sig /= 3309 -> err KSyscallErrorCodeInvalidSyscallInput 0
        | otherwise ->
            let gas = gasVerifySignatureMlDsa65 (fromIntegral (B.length msg))
             in if remaining <= gas
                  then hostErr HostFnErrorCode_OutOfGas remaining
                  else
                    if mldsa65Verify pk sig msg
                      then ok gas
                      else err KSyscallErrorCodeInvalidSyscallInput gas
      _ -> err KSyscallErrorCodeInvalidSyscallInput 0

------------------------------------------------------------------------------
-- Invocation

sysGetMsgSender :: SystemCallHandler -> IO (InnerResult, ContractAddress)
sysGetMsgSender h = do
  r <- getMsgSender h
  pure $ case r of
    Left e -> (err e 0, zeroHash)
    Right a -> (ok 0, a)

sysGetCurrentAddress :: SystemCallHandler -> IO (InnerResult, ContractAddress)
sysGetCurrentAddress h = (\a -> (ok 0, a)) <$> getCurrentAddress h

sysGetSourceTransactionHash :: SystemCallHandler -> IO (InnerResult, SHA256Hash)
sysGetSourceTransactionHash h = pure (ok 0, getSourceTxHash h)

sysGetCalldataLen :: SystemCallHandler -> IO (InnerResult, (Word32, Word32))
sysGetCalldataLen h = do
  cd <- getCalldata h
  n <- getParentSuppliedAuthTokenCount h
  pure (ok 0, (fromIntegral (B.length cd), n))

sysGetCalldata :: SystemCallHandler -> OutSpan -> IO (InnerResult, (Word32, Word32))
sysGetCalldata h buf = do
  cd <- getCalldata h
  written <- copyToGuestBuffer buf cd
  n <- getParentSuppliedAuthTokenCount h
  pure (ok 0, (written, n))

sysConsoleLog :: B.ByteString -> IO (InnerResult, ())
sysConsoleLog buf = do
  logTrace ("ConsoleLog: " ++ BC.unpack buf)
  pure (ok 0, ())

-- | CopyableInvokeOutputs.
data InvokeOutputs = InvokeOutputs
  { ioReturnBufferLen :: !Word32
  , ioReturnedAuthTokenCount :: !Word32
  , ioFrameStatus :: !InvocationFrameStatusCode
  }

zeroInvokeOutputs :: InvokeOutputs
zeroInvokeOutputs = InvokeOutputs 0 0 (InvocationFrameStatusCode 0)

invokeOutputsBytes :: InvokeOutputs -> B.ByteString
invokeOutputsBytes (InvokeOutputs l c (InvocationFrameStatusCode s)) = le32 l <> le32 c <> leI32 s

-- | ValidatedAuthTokens::parse: whole uint32 ids, no repeats, order kept.
parseAuthTokenIds :: B.ByteString -> SyscallStatus [Word32]
parseAuthTokenIds bytes
  | B.length bytes `mod` 4 /= 0 = Left KSyscallErrorCodeInvalidSyscallInput
  | otherwise =
      let ids = [fromLE32 (B.take 4 (B.drop (4 * i) bytes)) | i <- [0 .. B.length bytes `div` 4 - 1]]
          sorted = sort ids
       in if or (zipWith (==) sorted (drop 1 sorted)) then Left KSyscallErrorCodeInvalidSyscallInput else Right ids

flagMask :: InvokeFlag -> Word32
flagMask (InvokeFlag f) = fromIntegral f

sysInvoke
  :: SystemCallHandler
  -> Word64
  -> ContractAddress
  -> B.ByteString
  -> B.ByteString
  -> (Word64, Word64, Word32)
  -- ^ CopyableInvokeInputs: entrypoint id, gas limit, flags
  -> IO (InnerResult, InvokeOutputs)
sysInvoke h remaining addr calldata tokenBytes (entry, gasLimit, flags)
  | flags .&. (flagMask TryInvoke .|. flagMask ReadOnly) /= flags = pure (err KSyscallErrorCodeInvalidSyscallInput 0, zeroInvokeOutputs)
  | B.length tokenBytes > fromIntegral (configParams h).max_auth_tokens * 4 = pure (err KSyscallErrorCodeInvalidSyscallInput 0, zeroInvokeOutputs)
  | otherwise = case parseAuthTokenIds tokenBytes of
      Left e -> pure (err e 0, zeroInvokeOutputs)
      Right tokens -> do
        admitted <- validateNewInvocation h addr flags tokens
        case admitted of
          Left e -> pure (err e 0, zeroInvokeOutputs)
          Right ()
            | B.length calldata > fromIntegral (configParams h).max_calldata_len ->
                pure (err KSyscallErrorCodeInvalidSyscallInput 0, zeroInvokeOutputs)
            | otherwise -> do
                (res, gas) <- invoke h addr entry (min gasLimit remaining) calldata flags tokens
                case res of
                  Left e -> pure ((Left (systemToHostFn e), gas), zeroInvokeOutputs)
                  Right (Left e) -> pure (err e gas, zeroInvokeOutputs)
                  Right (Right status) -> do
                    rb <- getReturnBuffer h
                    case rb of
                      Left e -> pure ((Left (systemToHostFn e), gas), zeroInvokeOutputs)
                      Right buf -> do
                        (_, count) <- getReturnedAuthTokens h
                        let len = either (const 0) (fromIntegral . B.length) buf
                        pure (ok gas, InvokeOutputs len count status)

sysReturn :: SystemCallHandler -> B.ByteString -> Bool -> B.ByteString -> IO (InnerResult, ())
sysReturn h buf revert tokenBytes
  | B.length buf > fromIntegral (configParams h).max_return_buffer_len = exit
  | not (B.null tokenBytes) && revert = exit
  | B.length tokenBytes > fromIntegral (configParams h).max_auth_tokens * 4 = exit
  | otherwise = case parseAuthTokenIds tokenBytes of
      Left _ -> exit
      Right tokens -> do
        owned <- checkAuthTokensOwned h tokens
        if not owned
          then exit
          else do
            room <- checkParentCanTakeAuthTokens h (length tokens)
            if not room
              then exit
              else do
                setReturnBuffer h buf revert tokens
                pure (hostErr HostFnErrorCode_Return 0, ())
  where
    exit = pure (hostErr Exit 0, ())

sysGetReturnedBuffer :: SystemCallHandler -> OutSpan -> IO (InnerResult, (Word32, Word32, Word32))
sysGetReturnedBuffer h out = do
  r <- getReturnBuffer h
  case r of
    Left e -> pure (sysErr e, (0, 0, 0))
    Right (Left e) -> pure (err e 0, (0, 0, 0))
    Right (Right buf) -> do
      written <- copyToGuestBuffer out buf
      (start, count) <- getReturnedAuthTokens h
      pure (ok 0, (written, start, count))

sysExit :: IO (InnerResult, ())
sysExit = pure (hostErr Exit 0, ())

------------------------------------------------------------------------------
-- Contracts

sysGetDeployAddress :: SystemCallHandler -> Maybe ContractAddress -> SHA256Hash -> SHA256Hash -> IO (InnerResult, ContractAddress)
sysGetDeployAddress h caller nonce chash = do
  sender <- maybe (getCurrentAddress h) pure caller
  pure (ok 0, staticGetDeployAddress sender nonce chash)

sysDeployContract :: SystemCallHandler -> SHA256Hash -> SHA256Hash -> IO (InnerResult, ContractAddress)
sysDeployContract h nonce chash = do
  r <- deployContract h nonce chash
  pure $ case r of
    Left e -> (sysErr e, zeroHash)
    Right (Left e) -> (err e 0, zeroHash)
    Right (Right a) -> (ok 0, a)

------------------------------------------------------------------------------
-- Storage

sysBytestringGetLen :: SystemCallHandler -> StorageKeyContractData -> IO (InnerResult, Word32)
sysBytestringGetLen h key = do
  r <- getBytestring h key
  pure $ case r of
    Left e -> (sysErr e, 0)
    Right (StorageRead (Left e) lg) -> (err e (kStorageAccessCost + lg), 0)
    Right (StorageRead (Right b) lg) -> (ok (kStorageAccessCost + lg), fromIntegral (B.length b))

sysBytestringGet :: SystemCallHandler -> StorageKeyContractData -> OutSpan -> IO (InnerResult, Word32)
sysBytestringGet h key out = do
  r <- getBytestring h key
  case r of
    Left e -> pure (sysErr e, 0)
    Right (StorageRead (Left e) lg) -> pure (err e (kStorageAccessCost + lg), 0)
    Right (StorageRead (Right b) lg) -> do
      written <- copyToGuestBuffer out b
      -- The bytes copied, not the buffer offered.
      let gas = gasBytestringGet (fromIntegral (min (osLen out) (B.length b))) + lg
      pure (ok gas, written)

sysBytestringSet :: SystemCallHandler -> Word64 -> StorageKeyContractData -> B.ByteString -> IO (InnerResult, ())
sysBytestringSet h remaining key bytes = do
  let required = gasBytestringSet (fromIntegral (B.length bytes))
  if remaining <= required
    then pure (hostErr HostFnErrorCode_OutOfGas remaining, ())
    else do
      r <- setBytestring h key bytes
      pure $ case r of
        Left e -> (sysErr e, ())
        Right (Left e) -> (err e 0, ())
        Right (Right ()) -> (ok required, ())

-- | Outputs: current set value, whether it is present, current add value.
sysNonnegativeIntGet :: SystemCallHandler -> StorageKeyContractData -> Bool -> IO (InnerResult, (Integer, Bool, Integer))
sysNonnegativeIntGet h key force = do
  r <- getNnint h key force
  pure $ case r of
    Left e -> (sysErr e, (0, False, 0))
    Right (StorageRead (Left e) lg) -> (err e (kStorageAccessCost + lg), (0, False, 0))
    Right (StorageRead (Right (set, add)) lg) ->
      (ok (kStorageAccessCost + lg), (maybe 0 id set, maybe False (const True) set, add))

sysNonnegativeIntSetAdd :: SystemCallHandler -> StorageKeyContractData -> Maybe Integer -> Integer -> IO (InnerResult, ())
sysNonnegativeIntSetAdd h key set add = do
  r <- setNnint h key set add
  pure $ case r of
    Left e -> (sysErr e, ())
    Right (Left e) -> (err e kStorageAccessCost, ())
    Right (Right ()) -> (ok kStorageAccessCost, ())

sysDeleteObject :: SystemCallHandler -> StorageKeyContractData -> IO (InnerResult, ())
sysDeleteObject h key = do
  r <- deleteObject h key
  pure $ case r of
    Left e -> (sysErr e, ())
    Right (Left e) -> (err e kStorageAccessCost, ())
    Right (Right ()) -> (ok kStorageAccessCost, ())

sysSetStorageFlags :: SystemCallHandler -> StorageKeyContractData -> Word32 -> IO (InnerResult, ())
sysSetStorageFlags h key flags = do
  r <- setStorageFlags h key flags
  pure $ case r of
    Left e -> (err e kStorageAccessCost, ())
    Right () -> (ok kStorageAccessCost, ())

sysGetStorageFlags :: SystemCallHandler -> StorageKeyContractData -> IO (InnerResult, Word32)
sysGetStorageFlags h key = (\f -> (ok kStorageAccessCost, f)) <$> getStorageFlags h key

sysClearStorageFlags :: SystemCallHandler -> StorageKeyContractData -> IO (InnerResult, ())
sysClearStorageFlags h key = clearStorageFlags h key >> pure (ok kStorageAccessCost, ())

sysRetrocausalQueueInsert :: SystemCallHandler -> StorageKeyContractData -> B.ByteString -> IO (InnerResult, ())
sysRetrocausalQueueInsert h key entry = do
  r <- retrocausalQueueInsert h key entry
  pure $ case r of
    Left e -> (sysErr e, ())
    Right (Left e) -> (err e 0, ())
    Right (Right ()) -> (ok 0, ())

sysRetrocausalQueueConsume :: SystemCallHandler -> StorageKeyContractData -> Bool -> OutSpan -> IO (InnerResult, Word32)
sysRetrocausalQueueConsume h key peek out = do
  r <- retrocausalQueueConsume h key peek
  case r of
    Left e -> pure (sysErr e, 0)
    Right (Left e) -> pure (err e 0, 0)
    Right (Right entry) -> do
      written <- copyToGuestBuffer out entry
      pure (ok 0, written)

sysRetrocausalQueuePeekLen :: SystemCallHandler -> StorageKeyContractData -> IO (InnerResult, Word32)
sysRetrocausalQueuePeekLen h key = do
  r <- retrocausalQueueConsume h key True
  pure $ case r of
    Left e -> (sysErr e, 0)
    Right (Left e) -> (err e 0, 0)
    Right (Right entry) -> (ok 0, fromIntegral (B.length entry))

sysGetTransactionStatistics :: Word64 -> Int32 -> IO (InnerResult, Word64)
sysGetTransactionStatistics remaining stat = pure $ case TransactionStatistic stat of
  GasRemaining -> (ok 0, remaining)
  _ -> (err KSyscallErrorCodeInvalidSyscallInput 0, 0)

------------------------------------------------------------------------------
-- Events

sysLogEvent :: SystemCallHandler -> Maybe Topic -> Maybe Topic -> Maybe Topic -> B.ByteString -> IO (InnerResult, ())
sysLogEvent h t0 t1 t2 dat
  | (present t1 && not (present t0)) || (present t2 && (not (present t1) || not (present t0))) =
      pure (err KSyscallErrorCodeInvalidSyscallInput 0, ())
  | otherwise = do
      r <- logEvent h (concatMap (maybe [] pure) [t0, t1, t2]) dat
      pure $ case r of
        Left e -> (err e 0, ())
        Right () -> (ok 0, ())
  where
    present = maybe False (const True)

------------------------------------------------------------------------------
-- Authentication

sysChangePublicKeyRegistration
  :: SystemCallHandler -> Word32 -> Maybe Int32 -> B.ByteString -> Maybe Word32 -> IO (InnerResult, ())
sysChangePublicKeyRegistration h idx scheme keyBytes weight = do
  let newKey = case (scheme, weight) of
        (Nothing, Just _) -> Left KSyscallErrorCodeInvalidSyscallInput
        (Nothing, Nothing)
          | not (B.null keyBytes) -> Left KSyscallErrorCodeInvalidSyscallInput
          | otherwise -> Right Nothing
        (Just _, Nothing) -> Left KSyscallErrorCodeInvalidSyscallInput
        (Just s, Just w) -> case CryptographicScheme s of
          Ed25519
            | B.length keyBytes /= 32 -> Left KSyscallErrorCodeInvalidSyscallInput
            | otherwise -> Right (Just PublicKeyConfig {key = PublicKey_ed25519 keyBytes, max_weight = w})
          MlDsa65
            | B.length keyBytes /= 1952 -> Left KSyscallErrorCodeInvalidSyscallInput
            | otherwise -> Right (Just PublicKeyConfig {key = PublicKey_mldsa65 keyBytes, max_weight = w})
          _ -> Left KSyscallErrorCodeInvalidSyscallInput
  case newKey of
    Left e -> pure (err e 0, ())
    Right k -> do
      r <- changePublicKeyRegistration h idx k
      pure $ case r of
        Left e -> (sysErr e, ())
        Right (Left e) -> (err e 0, ())
        Right (Right ()) -> (ok 0, ())

sysWriteAddressRequiredWeight :: SystemCallHandler -> Word32 -> IO (InnerResult, ())
sysWriteAddressRequiredWeight h w = do
  r <- setAddressRequiredWeight h w
  pure $ case r of
    Left e -> (sysErr e, ())
    Right (Left e) -> (err e 0, ())
    Right (Right ()) -> (ok 0, ())

-- | generated::AuthClaim, the 48-byte repr(C) union the guest reads.
authClaimFfiBytes :: AuthClaim -> B.ByteString
authClaimFfiBytes = \case
  AuthClaim_source_address_claim c ->
    leI32 0 <> le32 c.allowed_keys <> le32 c.required_weight <> B.replicate 36 0
  AuthClaim_pubkey_preimage p ->
    leI32 1 <> B.replicate 4 0 <> p.pubkey_hash <> le64 p.nonce

sysGetAuthClaim :: SystemCallHandler -> IO (InnerResult, B.ByteString)
sysGetAuthClaim h = pure (ok 0, authClaimFfiBytes (getAuthClaim h))

sysMintAuthToken :: SystemCallHandler -> Word64 -> B.ByteString -> IO (InnerResult, Word32)
sysMintAuthToken h expiry serialized = do
  me <- getCurrentAddress h
  height <- getBlockHeight h
  if expiry < height
    then pure (err KSyscallErrorCodeInvalidSyscallInput 0, 0)
    else case xdrDecodeWith (fromIntegral (configParams h).max_auth_token_depth) serialized of
      Left _ -> pure (err KSyscallErrorCodeInvalidSyscallInput 0, 0)
      Right node -> do
        r <- mintAuthToken h AuthToken {mint_address = me, expiry_height = expiry, root_node = node}
        pure $ case r of
          Left e -> (err e 0, 0)
          Right tid -> (ok 0, tid)

-- | Outputs: minter, written payload length, unwrapped id start and count.
sysConsumeAuthToken :: SystemCallHandler -> Word32 -> OutSpan -> IO (InnerResult, (ContractAddress, Word32, Word32, Word32))
sysConsumeAuthToken h tid out = do
  height <- getBlockHeight h
  r <- consumeAuthToken h tid height
  case r of
    Left e -> pure (err e 0, (zeroHash, 0, 0, 0))
    Right res -> do
      written <- copyToGuestBuffer out (catToken res).root_node.payload
      pure (ok 0, ((catToken res).mint_address, written, catNewIdStart res, catNewIdCount res))

------------------------------------------------------------------------------
-- Block data

sysGetBlockHeight :: SystemCallHandler -> IO (InnerResult, Word64)
sysGetBlockHeight h = (\x -> (ok 0, x)) <$> getBlockHeight h

sysGetNetworkHash :: SystemCallHandler -> IO (InnerResult, NetworkHash)
sysGetNetworkHash h = (\x -> (ok 0, x)) <$> getNetworkHash h

sysGetBlockTimepoint :: SystemCallHandler -> IO (InnerResult, Word64)
sysGetBlockTimepoint h = (\x -> (ok 0, x)) <$> getBlockTimepoint h

------------------------------------------------------------------------------
-- The RV32 dispatch table

-- | A syscall as the sandbox calls it: parse arguments, run, and return the
-- result along with the outputs to flush.
data Rv32Syscall = forall a.
  Rv32Syscall
  { rsMeta :: SyscallMeta
  , rsParse :: ArgParser a
  , rsRun :: SystemCallHandler -> Word64 -> a -> IO (InnerResult, [B.ByteString])
  -- ^ outputs for the by-reference output arguments, in order
  }

meta :: SyscallNo -> Bool -> Bool -> SyscallMeta
meta = SyscallMeta

rw, roMeta, noReturn :: SyscallNo -> SyscallMeta
roMeta n = meta n True True
rw n = meta n False True
noReturn n = meta n True False

out1 :: (r, a) -> (a -> B.ByteString) -> (r, [B.ByteString])
out1 (r, a) f = (r, [f a])

storageKey :: ArgParser StorageKeyContractData
storageKey = pRefIn 32

int128In :: B.ByteString -> Integer
int128In = int128FromLE

syscallTable :: M.Map Word32 Rv32Syscall
syscallTable =
  M.fromList
    [ entry KSHA256 (roMeta KSHA256) ((,) <$> pSpanIn <*> pRefOut 32) $ \h g (i, _) -> (`out1` id) <$> sysSHA256 h g i
    , entry KGetMsgSender (roMeta KGetMsgSender) (pRefOut 32) $ \h _ _ -> (`out1` id) <$> sysGetMsgSender h
    , entry KGetCalldataLen (roMeta KGetCalldataLen) ((,) <$> pRefOut 4 <*> pRefOut 4) $ \h _ _ ->
        (\(r, (a, b)) -> (r, [le32 a, le32 b])) <$> sysGetCalldataLen h
    , entry KGetCalldata (roMeta KGetCalldata) ((,,) <$> pSpanOut <*> pRefOut 4 <*> pRefOut 4) $ \h _ (buf, _, _) ->
        (\(r, (a, b)) -> (r, [le32 a, le32 b])) <$> sysGetCalldata h buf
    , entry KConsoleLog (roMeta KConsoleLog) pSpanIn $ \_ _ b -> (\(r, ()) -> (r, [])) <$> sysConsoleLog b
    , entry
        KInvoke
        (roMeta KInvoke)
        ((,,,,) <$> pRefIn 32 <*> pSpanIn <*> pSpanIn <*> pRefIn 24 <*> pRefOut 12)
        $ \h g (addr, cd, toks, inputs, _) -> do
          let ins = (fromLE64 inputs, fromLE64 (B.drop 8 inputs), fromLE32 (B.drop 16 inputs))
          (\(r, o) -> (r, [invokeOutputsBytes o])) <$> sysInvoke h g addr cd toks ins
    , entry KReturn (noReturn KReturn) ((,,) <$> pSpanIn <*> pBool <*> pSpanIn) $ \h _ (b, rev, toks) ->
        (\(r, ()) -> (r, [])) <$> sysReturn h b rev toks
    , entry KGetReturnedBuffer (roMeta KGetReturnedBuffer) ((,,,) <$> pSpanOut <*> pRefOut 4 <*> pRefOut 4 <*> pRefOut 4) $
        \h _ (buf, _, _, _) -> (\(r, (a, b, c)) -> (r, [le32 a, le32 b, le32 c])) <$> sysGetReturnedBuffer h buf
    , entry KExit (noReturn KExit) (pure ()) $ \_ _ () -> (\(r, ()) -> (r, [])) <$> sysExit
    , entry
        KGetDeployAddress
        (roMeta KGetDeployAddress)
        ((,,,) <$> pOptRefIn 32 <*> pRefIn 32 <*> pRefIn 32 <*> pRefOut 32)
        $ \h _ (c, n, ch, _) -> (`out1` id) <$> sysGetDeployAddress h c n ch
    , entry KDeployContract (rw KDeployContract) ((,,) <$> pRefIn 32 <*> pRefIn 32 <*> pRefOut 32) $ \h _ (n, ch, _) ->
        (`out1` id) <$> sysDeployContract h n ch
    , entry KGetSourceTransactionHash (roMeta KGetSourceTransactionHash) (pRefOut 32) $ \h _ _ ->
        (`out1` id) <$> sysGetSourceTransactionHash h
    , entry KVerifySignature (roMeta KVerifySignature) ((,,,) <$> pInt32 <*> pSpanIn <*> pSpanIn <*> pSpanIn) $
        \_ g (s, sig, pk, msg) -> (\(r, ()) -> (r, [])) <$> sysVerifySignature g s sig pk msg
    , entry KBytestringGetLen (roMeta KBytestringGetLen) ((,) <$> storageKey <*> pRefOut 4) $ \h _ (k, _) ->
        (`out1` le32) <$> sysBytestringGetLen h k
    , entry KBytestringGet (roMeta KBytestringGet) ((,,) <$> storageKey <*> pSpanOut <*> pRefOut 4) $ \h _ (k, buf, _) ->
        (`out1` le32) <$> sysBytestringGet h k buf
    , entry KBytestringSet (rw KBytestringSet) ((,) <$> storageKey <*> pSpanIn) $ \h g (k, b) ->
        (\(r, ()) -> (r, [])) <$> sysBytestringSet h g k b
    , entry
        KNonnegativeIntGet
        (roMeta KNonnegativeIntGet)
        ((,,,,) <$> storageKey <*> pBool <*> pRefOut 16 <*> pRefOut 1 <*> pRefOut 16)
        $ \h _ (k, force, _, _, _) ->
          (\(r, (s, so, a)) -> (r, [int128ToLE s, B.singleton (if so then 1 else 0), int128ToLE a])) <$> sysNonnegativeIntGet h k force
    , entry KNonnegativeIntSetAdd (rw KNonnegativeIntSetAdd) ((,,) <$> storageKey <*> pOptRefIn 16 <*> pRefIn 16) $
        \h _ (k, s, a) -> (\(r, ()) -> (r, [])) <$> sysNonnegativeIntSetAdd h k (int128In <$> s) (int128In a)
    , entry KDeleteObject (rw KDeleteObject) storageKey $ \h _ k -> (\(r, ()) -> (r, [])) <$> sysDeleteObject h k
    , entry KGetCurrentAddress (roMeta KGetCurrentAddress) (pRefOut 32) $ \h _ _ -> (`out1` id) <$> sysGetCurrentAddress h
    , entry KRetrocausalQueueInsert (rw KRetrocausalQueueInsert) ((,) <$> storageKey <*> pSpanIn) $ \h _ (k, e) ->
        (\(r, ()) -> (r, [])) <$> sysRetrocausalQueueInsert h k e
    , entry
        KRetrocausalQueueConsume
        (rw KRetrocausalQueueConsume)
        ((,,,) <$> storageKey <*> pBool <*> pSpanOut <*> pRefOut 4)
        $ \h _ (k, peek, buf, _) -> (`out1` le32) <$> sysRetrocausalQueueConsume h k peek buf
    , entry KGetTransactionStatistics (roMeta KGetTransactionStatistics) ((,) <$> pInt32 <*> pRefOut 8) $ \_ g (s, _) ->
        (`out1` le64) <$> sysGetTransactionStatistics g s
    , entry
        KChangePublicKeyRegistration
        (rw KChangePublicKeyRegistration)
        ((,,,) <$> pWord32 <*> pOptRefIn 4 <*> pSpanIn <*> pOptRefIn 4)
        $ \h _ (idx, s, kb, w) ->
          (\(r, ()) -> (r, [])) <$> sysChangePublicKeyRegistration h idx (fromIntegral . fromLE32 <$> s) kb (fromLE32 <$> w)
    , entry KLogEvent (rw KLogEvent) ((,,,) <$> pOptRefIn 32 <*> pOptRefIn 32 <*> pOptRefIn 32 <*> pSpanIn) $
        \h _ (t0, t1, t2, d) -> (\(r, ()) -> (r, [])) <$> sysLogEvent h t0 t1 t2 d
    , entry KGetAuthClaim (roMeta KGetAuthClaim) (pRefOut 48) $ \h _ _ -> (`out1` id) <$> sysGetAuthClaim h
    , entry KSHA256Update (roMeta KSHA256Update) ((,,) <$> pWord32 <*> pSpanIn <*> pBool) $ \h g (i, b, reset) ->
        (\(r, ()) -> (r, [])) <$> sysSHA256Update h g i b reset
    , entry KSHA256Finalize (roMeta KSHA256Finalize) ((,) <$> pWord32 <*> pRefOut 32) $ \h _ (i, _) ->
        (`out1` id) <$> sysSHA256Finalize h i
    , entry KWriteAddressRequiredWeight (rw KWriteAddressRequiredWeight) pWord32 $ \h _ w ->
        (\(r, ()) -> (r, [])) <$> sysWriteAddressRequiredWeight h w
    , entry KGetBlockHeight (roMeta KGetBlockHeight) (pRefOut 8) $ \h _ _ -> (`out1` le64) <$> sysGetBlockHeight h
    , entry KRetrocausalQueuePeekLen (rw KRetrocausalQueuePeekLen) ((,) <$> storageKey <*> pRefOut 4) $ \h _ (k, _) ->
        (`out1` le32) <$> sysRetrocausalQueuePeekLen h k
    , entry KMintAuthToken (rw KMintAuthToken) ((,,) <$> pRefIn 8 <*> pSpanIn <*> pRefOut 4) $ \h _ (e, s, _) ->
        (`out1` le32) <$> sysMintAuthToken h (fromLE64 e) s
    , entry
        KConsumeAuthToken
        (rw KConsumeAuthToken)
        ((,,,,,) <$> pWord32 <*> pSpanOut <*> pRefOut 32 <*> pRefOut 4 <*> pRefOut 4 <*> pRefOut 4)
        $ \h _ (tid, buf, _, _, _, _) ->
          (\(r, (m, w, s, c)) -> (r, [m, le32 w, le32 s, le32 c])) <$> sysConsumeAuthToken h tid buf
    , entry KSHA256Copy (roMeta KSHA256Copy) ((,) <$> pWord32 <*> pWord32) $ \h _ (s, d) ->
        (\(r, ()) -> (r, [])) <$> sysSHA256Copy h s d
    , entry KGetNetworkHash (roMeta KGetNetworkHash) (pRefOut 32) $ \h _ _ -> (`out1` id) <$> sysGetNetworkHash h
    , entry KGetBlockTimepoint (roMeta KGetBlockTimepoint) (pRefOut 8) $ \h _ _ -> (`out1` le64) <$> sysGetBlockTimepoint h
    , entry KSetStorageFlags (rw KSetStorageFlags) ((,) <$> storageKey <*> pWord32) $ \h _ (k, f) ->
        (\(r, ()) -> (r, [])) <$> sysSetStorageFlags h k f
    , entry KGetStorageFlags (roMeta KGetStorageFlags) ((,) <$> storageKey <*> pRefOut 4) $ \h _ (k, _) ->
        (`out1` le32) <$> sysGetStorageFlags h k
    , entry KClearStorageFlags (rw KClearStorageFlags) storageKey $ \h _ k -> (\(r, ()) -> (r, [])) <$> sysClearStorageFlags h k
    ]
  where
    entry (SyscallNo n) m p run = (fromIntegral n, Rv32Syscall m p run)

syscallMetaOf :: Word32 -> Maybe SyscallMeta
syscallMetaOf n = (\(Rv32Syscall m _ _) -> m) <$> M.lookup n syscallTable

-- | syscall_entry: dispatch an ecall.  @a0@ is the syscall number and
-- @args@ are a1..a7.  Unknown numbers run the fallback, which reports
-- NoSyscall.
syscallEntry :: SystemCallHandler -> GuestMemory -> Word64 -> Word32 -> [Word32] -> IO SyscallResult
syscallEntry h mem gasRemaining no args = case M.lookup no syscallTable of
  Nothing ->
    fst
      <$> wrapAfterParse
        (SyscallMeta (SyscallNo (-1)) True True)
        True
        True
        h
        gasRemaining
        ()
        (\_ -> pure ((Right KSyscallErrorCodeNoSyscall, kNoSyscallCost), ()))
  Just (Rv32Syscall m p run) -> do
    parsed <- runArgParser mem args p
    let refs = paOutRefs parsed
        zero = [B.replicate (orSize r) 0 | r <- refs]
    (res, outs) <-
      wrapAfterParse m (paValid parsed) (paAliasOk parsed) h gasRemaining zero $ \g ->
        run h g (paValue parsed)
    case outs of
      Just os -> writeOutRefs mem (zip refs os)
      Nothing -> pure ()
    pure res

_unused :: ([Int] -> [Int], Word32 -> Bool)
_unused = (nub, (`testBit` 0))
