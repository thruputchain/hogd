-- | Contract method results and their abort-buffer encoding
-- (components/contract_result.h).
--
-- A contract method that "throws" ends its frame with an abort buffer laid
-- out as a CXS @u64@ exception selector followed by the CXS body of the
-- exception.  The selector of an exception type is the first eight bytes of
-- @sha256("<major>::<crate>::<TypeName>")@, read big-endian
-- ('calculateSelector'); 'kLibraryExceptionSelector' is the SDK's own
-- 'LibraryExn'.
--
-- @ContractErrorType<T...>@ becomes 'ContractErrorType' over an
-- 'ExceptionSet', the sum of a method's declared exceptions in the order the
-- generated trait lists them (which is the dispatch order).
module Hogd.Exec.Components.ContractResult
  ( -- * Signed tokens
    SignedToken (..)
  , kUnassignedHandle
    -- * Selectors
  , calculateSelector
  , kLibraryExceptionSelector
    -- * Exceptions
  , ExceptionSet (..)
  , NoExceptions
  , ContractErrorType (..)
  , fromAbortBuffer
  , serializeContractError
  , libraryExceptionBytes
  , exceptionBytes
    -- * Results
  , Result
  ) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.Bits (shiftL, (.|.))
import qualified Data.ByteString.Builder as BB
import qualified Data.ByteString.Lazy as BL
import Data.Word
import Hogd.Crypto (sha256)
import Hogd.Xdr
import X23.Xdr

-- | The C++ mirror of the SDK's @Signed<T>@: zero bytes on the wire; the
-- handle travels out of band and is assigned by the native contract builder
-- in declaration order.
newtype SignedToken = SignedToken {stHandle :: Word32}
  deriving (Eq, Show)

-- | @SignedToken::kUnassigned@.
kUnassignedHandle :: Word32
kUnassignedHandle = maxBound

-- | @calculate_selector@: the first eight digest bytes of
-- @sha256("<major>::<crate>::<type>")@ as a big-endian @u64@.
calculateSelector :: B.ByteString -> B.ByteString -> B.ByteString -> Word64
calculateSelector major crate typeName =
  let h = sha256 (B.concat [major, BC.pack "::", crate, BC.pack "::", typeName])
   in B.foldl' (\acc b -> (acc `shiftL` 8) .|. fromIntegral b) 0 (B.take 8 h)

-- | SHA256("1::sdk::LibraryExn")[0..8] as big-endian u64 (sdk_rs
-- exception_selector).
kLibraryExceptionSelector :: Word64
kLibraryExceptionSelector = 0xe562d59059c60ac3

-- | The exceptions a method declares (besides 'LibraryExn', which every
-- method can throw).
class ExceptionSet e where
  -- | The selector and CXS body of an exception.
  encodeException :: e -> (Word64, Builder)

  -- | The decoders to try, in dispatch order: the first whose selector
  -- matches decides.
  exceptionDecoders :: [(Word64, Get e)]

-- | For methods that declare no exceptions.
data NoExceptions
  deriving (Eq, Show)

instance ExceptionSet NoExceptions where
  encodeException e = case e of {}
  exceptionDecoders = []

-- | @ContractErrorType<T...>@.
data ContractErrorType e
  = ErrLibrary LibraryExn
  | ErrException e
  | -- | an abort buffer kept verbatim: an unknown selector, a body that did
    -- not decode, or a buffer forwarded without decoding
    -- (@from_raw_abort@)
    ErrRawAbort B.ByteString

deriving instance Eq e => Eq (ContractErrorType e)

deriving instance Show e => Show (ContractErrorType e)

-- | @Result<T, E...>@: Left is the error arm.
type Result e t = Either (ContractErrorType e) t

-- | The parsing constructor @ContractErrorType(ReturnBuffer &&)@: read the
-- selector, then dispatch on it.  A buffer whose selector does not parse,
-- whose selector is unknown, or whose body does not decode exactly is kept
-- raw.
fromAbortBuffer :: forall e. ExceptionSet e => B.ByteString -> ContractErrorType e
fromAbortBuffer buf = case runGet deserializationRecursionLimit cxsGetWord64 buf of
  Left _ -> ErrRawAbort buf
  Right (selector, used) ->
    let body = B.drop used buf
        decodeExact :: Get a -> Maybe a
        decodeExact g = case runGet deserializationRecursionLimit g body of
          Right (v, n) | n == B.length body -> Just v
          _ -> Nothing
        candidates =
          (kLibraryExceptionSelector, ErrLibrary <$> cxsGet)
            : [(s, ErrException <$> g) | (s, g) <- exceptionDecoders @e]
     in case [g | (s, g) <- candidates, s == selector] of
          (g : _) -> maybe (ErrRawAbort buf) id (decodeExact g)
          [] -> ErrRawAbort buf

-- | @ContractErrorType::serialize@: (u64 selector, CXS body), or the raw
-- buffer.
serializeContractError :: ExceptionSet e => ContractErrorType e -> B.ByteString
serializeContractError = \case
  ErrRawAbort b -> b
  ErrLibrary exn -> libraryExceptionBytes exn
  ErrException e -> let (s, body) = encodeException e in toStrict (cxsPutWord64 s <> body)

-- | @serialize_cxs(kLibraryExceptionSelector, exn)@.
libraryExceptionBytes :: LibraryExn -> B.ByteString
libraryExceptionBytes exn = toStrict (cxsPutWord64 kLibraryExceptionSelector <> cxsPut exn)

-- | @serialize_cxs(selector, exn)@ for an exception body.
exceptionBytes :: XdrType a => Word64 -> a -> B.ByteString
exceptionBytes s exn = toStrict (cxsPutWord64 s <> cxsPut exn)

toStrict :: Builder -> B.ByteString
toStrict = BL.toStrict . BB.toLazyByteString
