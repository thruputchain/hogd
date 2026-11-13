-- | Building native contracts from typed method bodies
-- (precompiles/native_contract_builder.{h,cc}).
--
-- A method is registered with its argument decoder; the builder's wrapper
-- mirrors the SDK entrypoint glue an RV32 contract links:
--
-- * the calldata is decoded in the trait's format; a decode failure, trailing
--   bytes, or a forwarded auth-token count that differs from the method's
--   'SignedToken' arguments aborts the frame with
--   @LibraryExn::Handling(DecodingArgs)@;
-- * a plain return value is encoded in the trait's format as the return
--   buffer;
-- * the error arm of a Result is encoded as (u64 selector, CXS body) and
--   aborts the frame;
-- * an 'InvokeError' from the body ends the frame with that error.
module Hogd.Exec.Precompiles.NativeContractBuilder
  ( -- * Methods
    ArgSpec (..)
  , noArgs
  , MethodReturn (..)
  , returnValue
  , returnUnit
  , returnResult
  , wrapMethod
  , traitMethod
  , traitAlwaysErrorMethod
  , kDefaultSerializationFormat
    -- * Building
  , buildNativeContract
  , checkMethods
    -- * Error helpers
  , hostFnErrorConversion
  , syscallException
  , decodingArgsAbortBytes
  ) where

import Control.Exception (SomeException, evaluate, try)
import qualified Data.ByteString as B
import Data.ByteString.Builder (Builder)
import qualified Data.Map.Strict as M
import Data.Word
import Hogd.Exec.Common.Status
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Log
import Hogd.Exec.Runtime.ContractTrait
import Hogd.Exec.Runtime.NativeRuntime
import Hogd.Exec.Runtime.SyscallHandler (getCalldata, getParentSuppliedAuthTokenCount, setReturnBuffer)
import Hogd.Exec.Runtime.Types (MethodID)
import Hogd.Xdr
import X23.Xdr (Get)

-- | @NativeContractBuilder::kDefaultSerializationFormat@, for methods added
-- by id rather than through a trait.
kDefaultSerializationFormat :: SerializationFormat
kDefaultSerializationFormat = Txs

-- | A method's argument tuple: its TXS and CXS decoders, and how many
-- top-level 'SignedToken' arguments it has.  Handles are the frame's ids
-- @0..N-1@, assigned in declaration order by 'asAssign'.
data ArgSpec args = ArgSpec
  { asXdr :: Get args
  , asCxs :: Get args
  , asSignedTokens :: Int
  , asAssign :: [SignedToken] -> args -> args
  }

-- | A method without arguments.
noArgs :: ArgSpec ()
noArgs = ArgSpec (pure ()) (pure ()) 0 (const id)

-- | What a method body produced.
data MethodReturn
  = -- | an encoded return value
    MethodReturn B.ByteString
  | -- | an encoded (selector, body) abort
    MethodRevert B.ByteString
  deriving (Eq, Show)

-- | A plain return value, given both its encoders.
returnValue :: SerializationFormat -> (Builder, Builder) -> MethodReturn
returnValue fmt enc = MethodReturn (encodeWith fmt enc)

-- | @do_return(std::tuple<>{})@: zero bytes in either format.
returnUnit :: MethodReturn
returnUnit = MethodReturn B.empty

-- | A Result: the ok arm as a return value, the error arm as an abort.
returnResult :: ExceptionSet e => SerializationFormat -> (a -> (Builder, Builder)) -> Result e a -> MethodReturn
returnResult fmt enc = \case
  Right v -> returnValue fmt (enc v)
  Left err -> MethodRevert (serializeContractError err)

-- | TODO(geoff) in the C++: the gas native argument-decoding errors cost.
kErrorGas :: Word64
kErrorGas = 0

-- | @cxs(kLibraryExceptionSelector, LibraryExn::Handling(DecodingArgs))@.
decodingArgsAbortBytes :: B.ByteString
decodingArgsAbortBytes = libraryExceptionBytes (LibraryExn_handling HandlingError_kHandlingDecodingArgs)

-- | The wrapper @NativeContractBuilder::add<Format>@ puts around a body.
wrapMethod
  :: SerializationFormat
  -> ArgSpec args
  -> (NativeSyscallContext -> args -> IO (InvokeStatus MethodReturn))
  -> NativeFunction
wrapMethod fmt spec body ctx = do
  r <- try (run >>= evaluate)
  case r of
    Right v -> pure v
    Left (e :: SomeException) -> do
      logError ("unrecoverable: unexpected exception in native contract: " ++ show e)
      pure (Left InvokeErrorCode_Unrecoverable)
  where
    h = nscHandler ctx
    decodingArgsAbort = do
      _ <- nscConsumeGas ctx kErrorGas
      setReturnBuffer h decodingArgsAbortBytes True []
      pure (Right ())
    run = do
      calldata <- getCalldata h
      case decodeWith fmt (asXdr spec, asCxs spec) calldata of
        Nothing -> decodingArgsAbort
        Just args0 -> do
          handleCount <- getParentSuppliedAuthTokenCount h
          if fromIntegral handleCount /= asSignedTokens spec
            then decodingArgsAbort
            else do
              let args = asAssign spec [SignedToken i | i <- [0 .. handleCount - 1]] args0
              ret <- body ctx args
              case ret of
                Left e -> pure (Left e)
                Right (MethodReturn buf) -> do
                  setReturnBuffer h buf False []
                  pure (Right ())
                Right (MethodRevert buf) -> do
                  setReturnBuffer h buf True []
                  pure (Right ())

-- | @builder.add<Trait, Name>(fn)@: keyed by the trait's method id and
-- decoded in the trait's format.
traitMethod
  :: ContractTrait
  -> B.ByteString
  -> ArgSpec args
  -> (NativeSyscallContext -> args -> IO (InvokeStatus MethodReturn))
  -> (MethodID, NativeFunction)
traitMethod trait name spec body = (traitMethodId trait name, wrapMethod (ctFormat trait) spec body)

-- | @kAlwaysErrorMethod<Trait, Name, Gas>@: decodes the method's arguments
-- like any other method, then consumes @gas@ and fails deterministically.
traitAlwaysErrorMethod :: ContractTrait -> B.ByteString -> ArgSpec args -> Word64 -> (MethodID, NativeFunction)
traitAlwaysErrorMethod trait name spec gas =
  traitMethod trait name spec $ \ctx _ -> do
    _ <- nscConsumeGas ctx gas
    pure (Left Deterministic)

-- | @NativeContractBuilder::build@.  A repeated method id is a
-- registration-time programming error (a panic in C++).
buildNativeContract :: [(MethodID, NativeFunction)] -> NativeContract
buildNativeContract fns =
  case [i | (i, n) <- M.toList (M.fromListWith (+) [(i, 1 :: Int) | (i, _) <- fns]), n > 1] of
    (dup : _) -> error ("Method ID " ++ show dup ++ " already exists")
    [] -> mkNativeContract fns

-- | @check_methods<Trait>@: every entrypoint of the trait is registered.
checkMethods :: ContractTrait -> [(MethodID, NativeFunction)] -> Bool
checkMethods trait fns = all (`elem` map fst fns) (map (traitMethodId trait) (ctExportNames trait))

-- | @NativeContractBuilder::host_fn_error_conversion@.
hostFnErrorConversion :: HostFnError -> IO InvokeError
hostFnErrorConversion = \case
  HostFnErrorCode_OutOfGas -> pure InvokeErrorCode_OutOfGas
  HostFnErrorCode_Unrecoverable -> pure InvokeErrorCode_Unrecoverable
  HostFnErrorCode_Abandon -> pure InvokeErrorCode_Abandon
  HostFnErrorCode_ValidationFailure -> pure InvokeErrorCode_ValidationFailure
  Exit -> pure Deterministic
  HostFnErrorCode_Return -> do
    logError "unrecoverable: contracts using the NativeContractBuilder should not call the Return syscall directly"
    pure InvokeErrorCode_Unrecoverable
  NoneOrRecoverable -> do
    logError "unrecoverable: host_fn_error_conversion: NoneOrRecoverable when error expected"
    pure InvokeErrorCode_Unrecoverable
  other -> do
    logError ("unrecoverable: host_fn_error_conversion: unknown HostFnErrorCode: " ++ show other)
    pure InvokeErrorCode_Unrecoverable

-- | @NativeContractBuilder::syscall_exception@: the @LibraryExn::Syscall@
-- abort the SDK throws when a syscall reports an error
-- (@or_syscall_throw@).
syscallException :: SyscallNo -> SyscallErrorCode -> LibraryExn
syscallException (SyscallNo call) (SyscallErrorCode err) =
  LibraryExn_syscall_exn SyscallExn {call_number = fromIntegral call, err_number = err}
