-- | Calldata/return-value codecs for the types contract specs use
-- (built_contracts/generate_header.py's type mapping), in both
-- serialization formats.
--
-- The spec type kinds map as the generated C++ headers map them:
--
-- * @address@ -> @ContractAddress@ (32 raw bytes)
-- * @i128@ -> @std::tuple<int64_t, uint64_t>@ (an @int128_xdr@ on the wire)
-- * @array<u8, N>@ -> @std::array<uint8_t, N>@ (N raw bytes)
-- * @vec<u8, M>@ -> @x23::Vec<uint8_t, M>@ (length-prefixed bytes)
-- * @vec<T, M>@, @option<T>@, tuples and structs as XDR does
-- * @signed_token@ -> zero bytes
module Hogd.BuiltContracts.Codec
  ( Codec (..)
  , encodeC
  , decodeC
  , codecPair
  , argSpec
  , argSpecSigned
    -- * Primitive codecs
  , unitC
  , boolC
  , u32C
  , u64C
  , i32C
  , i128C
  , fixedOpaqueC
  , addressC
  , hashC
  , entrypointC
  , opaqueVecC
  , vecC
  , optionC
  , signedTokenC
    -- * Products
  , pairC
  , tripleC
  , quadC
  , mapC
    -- * int128 split
  , i128Split
  , i128Join
  ) where

import Data.Bits
import qualified Data.ByteString as B
import Data.ByteString.Builder (Builder)
import Data.Int
import Data.Word
import Hogd.Exec.Components.ContractResult (SignedToken (..), kUnassignedHandle)
import Hogd.Exec.Precompiles.NativeContractBuilder (ArgSpec (..))
import Hogd.Exec.Runtime.ContractTrait
import X23.Xdr

-- | Both encodings of a type.
data Codec a = Codec
  { cXdrPut :: a -> Builder
  , cXdrGet :: Get a
  , cCxsPut :: a -> Builder
  , cCxsGet :: Get a
  }

encodeC :: SerializationFormat -> Codec a -> a -> B.ByteString
encodeC fmt c v = encodeWith fmt (codecPair c v)

decodeC :: SerializationFormat -> Codec a -> B.ByteString -> Maybe a
decodeC fmt c = decodeWith fmt (cXdrGet c, cCxsGet c)

-- | The (TXS, CXS) encoder pair 'returnValue' takes.
codecPair :: Codec a -> a -> (Builder, Builder)
codecPair c v = (cXdrPut c v, cCxsPut c v)

-- | An argument tuple with no signed tokens.
argSpec :: Codec a -> ArgSpec a
argSpec c = ArgSpec (cXdrGet c) (cCxsGet c) 0 (const id)

-- | An argument tuple with @n@ top-level signed tokens.
argSpecSigned :: Codec a -> Int -> ([SignedToken] -> a -> a) -> ArgSpec a
argSpecSigned c = ArgSpec (cXdrGet c) (cCxsGet c)

unitC :: Codec ()
unitC = Codec (const mempty) (pure ()) (const mempty) (pure ())

boolC :: Codec Bool
boolC = Codec putBool getBool cxsPutBool cxsGetBool

u32C :: Codec Word32
u32C = Codec putWord32 getWord32 cxsPutWord32 cxsGetWord32

i32C :: Codec Int32
i32C = Codec putInt32 getInt32 cxsPutInt32 cxsGetInt32

u64C :: Codec Word64
u64C = Codec putWord64 getWord64 cxsPutWord64 cxsGetWord64

-- | The (high, low) halves of an int128, as @std::tuple<int64_t, uint64_t>@.
-- Wraps modulo 2^128.
i128Split :: Integer -> (Int64, Word64)
i128Split v = (fromIntegral (v `shiftR` 64), fromIntegral (v .&. 0xffffffffffffffff))

-- | @from_wire@: @int128_t{hi} << 64 | lo@.
i128Join :: Int64 -> Word64 -> Integer
i128Join hi lo = toInteger hi * 2 ^ (64 :: Int) + toInteger lo

i128C :: Codec Integer
i128C =
  Codec
    (\v -> let (hi, lo) = i128Split v in putInt64 hi <> putWord64 lo)
    (i128Join <$> getInt64 <*> getWord64)
    (\v -> let (hi, lo) = i128Split v in cxsPutInt64 hi <> cxsPutWord64 lo)
    (i128Join <$> cxsGetInt64 <*> cxsGetWord64)

-- | @std::array<uint8_t, N>@ / @opaque[N]@.
fixedOpaqueC :: Int -> Codec B.ByteString
fixedOpaqueC n = Codec (putFixedOpaque n) (getFixedOpaque n) (cxsPutFixedOpaque n) (cxsGetFixedOpaque n)

addressC, hashC, entrypointC :: Codec B.ByteString
addressC = fixedOpaqueC 32
hashC = fixedOpaqueC 32
entrypointC = fixedOpaqueC 8

-- | @x23::Vec<uint8_t, bound>@.
opaqueVecC :: Word32 -> Codec B.ByteString
opaqueVecC bound = Codec (putVarOpaque bound) (getVarOpaque bound) (cxsPutVarOpaque bound) (cxsGetVarOpaque bound)

-- | @x23::Vec<T, bound>@.
vecC :: Word32 -> Codec a -> Codec [a]
vecC bound c =
  Codec
    (putVarArray bound (cXdrPut c))
    (getVarArray bound (cXdrGet c))
    (cxsPutVarArray bound (cCxsPut c))
    (cxsGetVarArray bound (cCxsGet c))

-- | @x23::Ptr<T>@.
optionC :: Codec a -> Codec (Maybe a)
optionC c = Codec (putOptional (cXdrPut c)) (getOptional (cXdrGet c)) (cxsPutOptional (cCxsPut c)) (cxsGetOptional (cCxsGet c))

-- | @Signed<T>@: zero bytes; the handle is assigned after decoding.
signedTokenC :: Codec SignedToken
signedTokenC = Codec (const mempty) (pure unassigned) (const mempty) (pure unassigned)
  where
    unassigned = SignedToken kUnassignedHandle

pairC :: Codec a -> Codec b -> Codec (a, b)
pairC a b =
  Codec
    (\(x, y) -> cXdrPut a x <> cXdrPut b y)
    ((,) <$> cXdrGet a <*> cXdrGet b)
    (\(x, y) -> cCxsPut a x <> cCxsPut b y)
    ((,) <$> cCxsGet a <*> cCxsGet b)

tripleC :: Codec a -> Codec b -> Codec c -> Codec (a, b, c)
tripleC a b c =
  Codec
    (\(x, y, z) -> cXdrPut a x <> cXdrPut b y <> cXdrPut c z)
    ((,,) <$> cXdrGet a <*> cXdrGet b <*> cXdrGet c)
    (\(x, y, z) -> cCxsPut a x <> cCxsPut b y <> cCxsPut c z)
    ((,,) <$> cCxsGet a <*> cCxsGet b <*> cCxsGet c)

quadC :: Codec a -> Codec b -> Codec c -> Codec d -> Codec (a, b, c, d)
quadC a b c d =
  Codec
    (\(w, x, y, z) -> cXdrPut a w <> cXdrPut b x <> cXdrPut c y <> cXdrPut d z)
    ((,,,) <$> cXdrGet a <*> cXdrGet b <*> cXdrGet c <*> cXdrGet d)
    (\(w, x, y, z) -> cCxsPut a w <> cCxsPut b x <> cCxsPut c y <> cCxsPut d z)
    ((,,,) <$> cCxsGet a <*> cCxsGet b <*> cCxsGet c <*> cCxsGet d)

-- | An isomorphic codec (e.g. a struct over its field tuple).
mapC :: (a -> b) -> (b -> a) -> Codec a -> Codec b
mapC to from c =
  Codec (cXdrPut c . from) (to <$> cXdrGet c) (cCxsPut c . from) (to <$> cCxsGet c)
