-- | Contract traits: entrypoint and interface id derivation, calldata
-- formats, and decoding a callee's return (runtime/contract_trait.{h,cc},
-- runtime/serialize_calldata.h).
--
-- The C++ generated trait headers (built_contracts/generate_header.py, from
-- each contract's spec JSON) become 'ContractTrait' values: a trait name, the
-- declaring crate's version, and the method names.  Entrypoint ids are
-- @sha256("<name>::<major>::<method>")[0..8]@; the method id the executor
-- dispatches on is those eight bytes read big-endian.
module Hogd.Exec.Runtime.ContractTrait
  ( -- * Derivation
    majorVersion
  , sdkEntrypointDerivation
  , sdkMethodIdFor
  , sdkMethodId
  , entrypointFromMethodId
  , InterfaceId
  , sdkInterfaceIdDerivation
    -- * Traits
  , ContractTrait (..)
  , traitEntrypoint
  , traitMethodId
  , traitInterfaceId
    -- * Calldata formats
  , SerializationFormat (..)
  , encodeWith
  , decodeWith
  , runBuilder
    -- * Decoding a return
  , decodeReturn
  , decodeResultReturn
  ) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import qualified Data.ByteString.Builder as BB
import qualified Data.ByteString.Lazy as BL
import Hogd.Crypto (sha256)
import Hogd.Exec.Components.ContractResult
import Hogd.Exec.Log (logError)
import Hogd.Exec.Runtime.SyscallHandler (entrypointFromMethodId, sdkMethodId)
import Hogd.Exec.Runtime.Types (MethodID)
import Hogd.Xdr (ContractEntrypointID)
import X23.Xdr

-- | The SDK's "major version" fold of a semver string (sdk_rs
-- contract-util::major_version, generate_header.py):
--
-- * major > 0 -> @MAJOR@
-- * major == 0, minor > 0 -> @MAJOR.MINOR@
-- * major == minor == 0, patch > 0 -> @MAJOR.MINOR.PATCH@
-- * all three zero -> the whole string, prerelease and build included
--
-- Always a prefix of the input.  Like the C++, a malformed version (no
-- digits where one is required) is a programming error.
majorVersion :: B.ByteString -> B.ByteString
majorVersion version =
  let (majorNz, afterMajor) = scan 0
   in if majorNz
        then B.take afterMajor version
        else
          let (minorNz, afterMinor) = scan (expectDot afterMajor + 1)
           in if minorNz
                then B.take afterMinor version
                else
                  let (patchNz, afterPatch) = scan (expectDot afterMinor + 1)
                   in if patchNz then B.take afterPatch version else version
  where
    isDigit b = b >= 0x30 && b <= 0x39
    scan start =
      let digits = B.takeWhile isDigit (B.drop start version)
       in if B.null digits
            then error ("majorVersion: no digits in " ++ show version)
            else (B.any (/= 0x30) digits, start + B.length digits)
    expectDot pos
      | pos < B.length version && B.index version pos == 0x2e = pos
      | otherwise = error ("majorVersion: malformed version " ++ show version)

sep :: B.ByteString
sep = BC.pack "::"

-- | @sdk_entrypoint_derivation@:
-- @sha256("<name>::<major>::<method>")[0..8]@.
sdkEntrypointDerivation :: B.ByteString -> B.ByteString -> B.ByteString -> ContractEntrypointID
sdkEntrypointDerivation name version method =
  B.take 8 (sha256 (B.concat [name, sep, majorVersion version, sep, method]))

-- | @sdk_method_id(name, version, method)@.
sdkMethodIdFor :: B.ByteString -> B.ByteString -> B.ByteString -> MethodID
sdkMethodIdFor name version method = sdkMethodId (sdkEntrypointDerivation name version method)

-- | An SDK interface id: what ERC-165's @supports_interface@ is asked
-- about.  Eight bytes.
type InterfaceId = B.ByteString

-- | @sdk_interface_id_derivation@:
-- @sha256("thruput-interface::<name>::<major>")[0..8]@.
sdkInterfaceIdDerivation :: B.ByteString -> B.ByteString -> InterfaceId
sdkInterfaceIdDerivation name version =
  B.take 8 (sha256 (B.concat [BC.pack "thruput-interface", sep, name, sep, majorVersion version]))

-- | A generated contract trait (one interface of a contract spec).
data ContractTrait = ContractTrait
  { ctName :: B.ByteString
  -- ^ kTraitName
  , ctVersion :: B.ByteString
  -- ^ kVersion: the declaring crate's version
  , ctFormat :: SerializationFormat
  -- ^ kSerializationFormat (generated traits are all CXS)
  , ctExportNames :: [B.ByteString]
  -- ^ kExportNames
  }
  deriving (Eq, Show)

-- | @ContractTraitMeta::entrypoint<Name>()@.  Unknown names are a
-- programming error, as they are a compile error in C++.
traitEntrypoint :: ContractTrait -> B.ByteString -> ContractEntrypointID
traitEntrypoint t method
  | method `elem` ctExportNames t = sdkEntrypointDerivation (ctName t) (ctVersion t) method
  | otherwise = error ("traitEntrypoint: " ++ show method ++ " is not a method of " ++ show (ctName t))

-- | @ContractTraitMeta::method_id<Name>()@.
traitMethodId :: ContractTrait -> B.ByteString -> MethodID
traitMethodId t = sdkMethodId . traitEntrypoint t

traitInterfaceId :: ContractTrait -> InterfaceId
traitInterfaceId t = sdkInterfaceIdDerivation (ctName t) (ctVersion t)

-- | @SerializationFormat@: TXS is XDR (4-byte aligned), CXS the compact
-- canonical encoding.
data SerializationFormat = Txs | Cxs
  deriving (Eq, Show)

runBuilder :: Builder -> B.ByteString
runBuilder = BL.toStrict . BB.toLazyByteString

-- | @Serializer<Format>::serialize@ given both encoders of a value.
encodeWith :: SerializationFormat -> (Builder, Builder) -> B.ByteString
encodeWith Txs (xdr, _) = runBuilder xdr
encodeWith Cxs (_, cxs) = runBuilder cxs

-- | @Serializer<Format>::deserialize@: the whole buffer must be consumed
-- (and, for TXS, be a multiple of four bytes).
decodeWith :: SerializationFormat -> (Get a, Get a) -> B.ByteString -> Maybe a
decodeWith fmt (xdrG, cxsG) bs = case fmt of
  Txs
    | B.length bs `rem` 4 /= 0 -> Nothing
    | otherwise -> exact xdrG
  Cxs -> exact cxsG
  where
    exact g = case runGet deserializationRecursionLimit g bs of
      Right (v, n) | n == B.length bs -> Just v
      _ -> Nothing

-- | @decode_return@ for a method whose return type is not a Result: a
-- manually reverted frame, or an ok value that does not decode, is Nothing.
decodeReturn :: (B.ByteString -> Maybe a) -> B.ByteString -> Bool -> IO (Maybe a)
decodeReturn decodeOk dat manuallyReverted
  | manuallyReverted = do
      logError "Decode Return Failed: Manually reverted in non-result return type"
      pure Nothing
  | otherwise = case decodeOk dat of
      Nothing -> do
        logError "Decode Return Failed: Failed to deserialize return value"
        pure Nothing
      Just v -> pure (Just v)

-- | @decode_return@ for a Result-returning method: an abort buffer becomes
-- the error arm (parsed by selector); otherwise the ok value must decode.
decodeResultReturn :: ExceptionSet e => (B.ByteString -> Maybe a) -> B.ByteString -> Bool -> IO (Maybe (Result e a))
decodeResultReturn decodeOk dat manuallyReverted
  | manuallyReverted = pure (Just (Left (fromAbortBuffer dat)))
  | otherwise = case decodeOk dat of
      Nothing -> do
        logError "Decode Return Failed: Failed to deserialize return ok type"
        pure Nothing
      Just v -> pure (Just (Right v))
