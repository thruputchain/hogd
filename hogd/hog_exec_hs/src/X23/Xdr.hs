{-# LANGUAGE BangPatterns #-}
{-# LANGUAGE RankNTypes #-}

-- | Runtime support for XDR (RFC 4506) as implemented by x23, plus the
-- "CXS" compact canonical serialization x23 uses for hashing.
--
-- Generated modules (see @xdrgen@) build on the primitives here.  Semantics
-- follow x23/serialize.h and x23/serialize_cxs.h:
--
--  * enums decode as open values (any int32 is accepted);
--  * bools must be 0 or 1;
--  * opaque padding must be zero;
--  * variable-length bounds are enforced on decode;
--  * recursive structs and unions share a nesting budget
--    ('deserializationRecursionLimit').
module X23.Xdr
  ( -- * Classes
    XdrType (..)
  , xdrEncode
  , xdrEncodeStrict
  , xdrDecode
  , xdrDecodeWith
  , xdrDecodePrefix
  , xdrSize
  , cxsEncode
  , cxsDecode
  , cxsSize
  , deserializationRecursionLimit
    -- * Put helpers
  , Builder
  , putInt32
  , putWord32
  , putInt64
  , putWord64
  , putBool
  , putFloat
  , putDouble
  , putFixedOpaque
  , putVarOpaque
  , putFixedArray
  , putVarArray
  , putOptional
    -- * Get monad
  , Get
  , runGet
  , getFail
  , getInt32
  , getWord32
  , getInt64
  , getWord64
  , getBool
  , getFloat
  , getDouble
  , getFixedOpaque
  , getVarOpaque
  , getFixedArray
  , getVarArray
  , getOptional
  , getRecursive
  , getRemaining
    -- * CXS helpers
  , cxsPutInt
  , cxsPutWord
  , cxsPutInt32
  , cxsPutWord32
  , cxsPutInt64
  , cxsPutWord64
  , cxsPutBool
  , cxsPutFloat
  , cxsPutDouble
  , cxsPutFixedOpaque
  , cxsPutVarOpaque
  , cxsPutFixedArray
  , cxsPutVarArray
  , cxsPutOptional
  , cxsGetInt32
  , cxsGetWord32
  , cxsGetInt64
  , cxsGetWord64
  , cxsGetBool
  , cxsGetFloat
  , cxsGetDouble
  , cxsGetFixedOpaque
  , cxsGetVarOpaque
  , cxsGetFixedArray
  , cxsGetVarArray
  , cxsGetOptional
  , CxsError (..)
  ) where

import Control.Monad (replicateM, unless, when)
import Data.Bits
import qualified Data.ByteString as B
import Data.ByteString.Builder (Builder)
import qualified Data.ByteString.Builder as BB
import qualified Data.ByteString.Lazy as BL
import qualified Data.ByteString.Unsafe as BU
import Data.Int
import Data.Word
import GHC.Float (castDoubleToWord64, castFloatToWord32, castWord32ToFloat, castWord64ToDouble)

-- | Types with an XDR and a CXS encoding.
class XdrType a where
  xdrPut :: a -> Builder
  xdrGet :: Get a
  cxsPut :: a -> Builder
  cxsGet :: Get a

-- | x23's default nesting limit for recursive types.
deserializationRecursionLimit :: Int
deserializationRecursionLimit = 16

xdrEncode :: XdrType a => a -> BL.ByteString
xdrEncode = BB.toLazyByteString . xdrPut

xdrEncodeStrict :: XdrType a => a -> B.ByteString
xdrEncodeStrict = BL.toStrict . xdrEncode

xdrSize :: XdrType a => a -> Int
xdrSize = fromIntegral . BL.length . xdrEncode

-- | Decode a complete buffer, as x23::deserialize does: the input must be a
-- multiple of four bytes and fully consumed.
xdrDecode :: XdrType a => B.ByteString -> Either String a
xdrDecode = xdrDecodeWith deserializationRecursionLimit

xdrDecodeWith :: XdrType a => Int -> B.ByteString -> Either String a
xdrDecodeWith lim bs
  | B.length bs `rem` 4 /= 0 = Left "input not multiple of 4 bytes"
  | otherwise = case runGet lim xdrGet bs of
      Left e -> Left e
      Right (a, n)
        | n /= B.length bs -> Left "leftover bytes"
        | otherwise -> Right a

-- | Decode a value from the front of a buffer, returning the bytes consumed.
xdrDecodePrefix :: XdrType a => B.ByteString -> Either String (a, Int)
xdrDecodePrefix = runGet deserializationRecursionLimit xdrGet

cxsEncode :: XdrType a => a -> B.ByteString
cxsEncode = BL.toStrict . BB.toLazyByteString . cxsPut

cxsSize :: XdrType a => a -> Int
cxsSize = B.length . cxsEncode

data CxsError = CxsTruncated | CxsNonCanonical | CxsBadDiscriminant | CxsLengthExceeded | CxsRecursionLimit | CxsTrailingData
  deriving (Eq, Show)

-- | Decode a complete CXS buffer.
cxsDecode :: XdrType a => B.ByteString -> Either String a
cxsDecode bs = case runGet deserializationRecursionLimit cxsGet bs of
  Left e -> Left e
  Right (a, n)
    | n /= B.length bs -> Left "trailing data"
    | otherwise -> Right a

------------------------------------------------------------------------------
-- Get

-- | A strict, CPS-encoded parser over a strict ByteString.  The 'Int'
-- threaded alongside the offset is the remaining recursion budget.
newtype Get a = Get
  { unGet
      :: forall r
       . B.ByteString
      -> Int -- offset
      -> Int -- recursion budget
      -> (String -> r)
      -> (a -> Int -> r)
      -> r
  }

instance Functor Get where
  fmap f (Get g) = Get $ \bs off d kf ks -> g bs off d kf (\a o -> ks (f a) o)
  {-# INLINE fmap #-}

instance Applicative Get where
  pure a = Get $ \_ off _ _ ks -> ks a off
  {-# INLINE pure #-}
  Get gf <*> Get ga = Get $ \bs off d kf ks ->
    gf bs off d kf (\f o -> ga bs o d kf (\a o' -> ks (f a) o'))
  {-# INLINE (<*>) #-}

instance Monad Get where
  Get g >>= k = Get $ \bs off d kf ks ->
    g bs off d kf (\a o -> unGet (k a) bs o d kf ks)
  {-# INLINE (>>=) #-}

instance MonadFail Get where
  fail = getFail

getFail :: String -> Get a
getFail msg = Get $ \_ _ _ kf _ -> kf msg

runGet :: Int -> Get a -> B.ByteString -> Either String (a, Int)
runGet lim (Get g) bs = g bs 0 lim Left (\a o -> Right (a, o))

-- | Bytes remaining in the input.
getRemaining :: Get Int
getRemaining = Get $ \bs off _ _ ks -> ks (B.length bs - off) off

need :: Int -> Get ()
need n = Get $ \bs off _ kf ks ->
  if n < 0 || B.length bs - off < n then kf "input too short" else ks () off
{-# INLINE need #-}

takeBytes :: Int -> Get B.ByteString
takeBytes n = Get $ \bs off _ kf ks ->
  if n < 0 || B.length bs - off < n
    then kf "input too short"
    else ks (B.copy (BU.unsafeTake n (BU.unsafeDrop off bs))) (off + n)
{-# INLINE takeBytes #-}

getByte :: Get Word8
getByte = Get $ \bs off _ kf ks ->
  if B.length bs - off < 1 then kf "input too short" else ks (BU.unsafeIndex bs off) (off + 1)

getBE :: Int -> Get Word64
getBE n = Get $ \bs off _ kf ks ->
  if B.length bs - off < n
    then kf "input too short"
    else
      let go !acc !i
            | i == n = acc
            | otherwise = go ((acc `shiftL` 8) .|. fromIntegral (BU.unsafeIndex bs (off + i))) (i + 1)
       in ks (go 0 0) (off + n)
{-# INLINE getBE #-}

getLE :: Int -> Get Word64
getLE n = Get $ \bs off _ kf ks ->
  if B.length bs - off < n
    then kf "truncated"
    else
      let go !acc !i
            | i < 0 = acc
            | otherwise = go ((acc `shiftL` 8) .|. fromIntegral (BU.unsafeIndex bs (off + i))) (i - 1)
       in ks (go 0 (n - 1)) (off + n)
{-# INLINE getLE #-}

-- | Run a decoder for a recursive type, charging one unit of nesting budget.
getRecursive :: Get a -> Get a
getRecursive (Get g) = Get $ \bs off d kf ks ->
  if d <= 0 then kf "exceeded recursion limit" else g bs off (d - 1) kf ks

------------------------------------------------------------------------------
-- XDR primitives

putInt32 :: Int32 -> Builder
putInt32 = BB.int32BE

putWord32 :: Word32 -> Builder
putWord32 = BB.word32BE

putInt64 :: Int64 -> Builder
putInt64 = BB.int64BE

putWord64 :: Word64 -> Builder
putWord64 = BB.word64BE

putBool :: Bool -> Builder
putBool b = BB.word32BE (if b then 1 else 0)

putFloat :: Float -> Builder
putFloat = BB.word32BE . castFloatToWord32

putDouble :: Double -> Builder
putDouble = BB.word64BE . castDoubleToWord64

pad :: Int -> Builder
pad n = case n .&. 3 of
  0 -> mempty
  r -> BB.byteString (B.replicate (4 - r) 0)

putFixedOpaque :: Int -> B.ByteString -> Builder
putFixedOpaque n bs
  | B.length bs /= n = error ("putFixedOpaque: expected " ++ show n ++ " bytes, got " ++ show (B.length bs))
  | otherwise = BB.byteString bs <> pad n

-- | Variable-length opaque/string; the bound is checked by the caller's
-- type (x23's Vec cannot hold more than its bound).
putVarOpaque :: Word32 -> B.ByteString -> Builder
putVarOpaque bound bs
  | fromIntegral (B.length bs) > bound = error ("putVarOpaque: length exceeds bound " ++ show bound)
  | otherwise = BB.word32BE (fromIntegral (B.length bs)) <> BB.byteString bs <> pad (B.length bs)

putFixedArray :: Int -> (a -> Builder) -> [a] -> Builder
putFixedArray n p xs
  | length xs /= n = error ("putFixedArray: expected " ++ show n ++ " elements")
  | otherwise = foldMap p xs

putVarArray :: Word32 -> (a -> Builder) -> [a] -> Builder
putVarArray bound p xs
  | fromIntegral len > bound = error ("putVarArray: length exceeds bound " ++ show bound)
  | otherwise = BB.word32BE (fromIntegral len) <> foldMap p xs
  where
    len = length xs

putOptional :: (a -> Builder) -> Maybe a -> Builder
putOptional _ Nothing = putBool False
putOptional p (Just a) = putBool True <> p a

getWord32 :: Get Word32
getWord32 = fromIntegral <$> getBE 4

getInt32 :: Get Int32
getInt32 = fromIntegral <$> getWord32

getWord64 :: Get Word64
getWord64 = getBE 8

getInt64 :: Get Int64
getInt64 = fromIntegral <$> getWord64

getBool :: Get Bool
getBool = do
  v <- getWord32
  case v of
    0 -> pure False
    1 -> pure True
    _ -> getFail ("illegal bool value " ++ show v ++ " in deserialization")

getFloat :: Get Float
getFloat = castWord32ToFloat <$> getWord32

getDouble :: Get Double
getDouble = castWord64ToDouble <$> getWord64

checkPad :: Int -> Get ()
checkPad n = case n .&. 3 of
  0 -> pure ()
  r -> do
    p <- takeBytes (4 - r)
    unless (B.all (== 0) p) $ getFail "padding bytes are not zero deserializing XDR"

getFixedOpaque :: Int -> Get B.ByteString
getFixedOpaque n = do
  need ((n + 3) .&. complement 3)
  bs <- takeBytes n
  checkPad n
  pure bs

getVarOpaque :: Word32 -> Get B.ByteString
getVarOpaque bound = do
  n <- getWord32
  when (n > bound) $ getFail ("length " ++ show n ++ " exceeds maximum")
  let n' = fromIntegral n
  need ((n' + 3) .&. complement 3)
  bs <- takeBytes n'
  checkPad n'
  pure bs

getFixedArray :: Int -> Get a -> Get [a]
getFixedArray = replicateM

getVarArray :: Word32 -> Get a -> Get [a]
getVarArray bound g = do
  n <- getWord32
  when (n > bound) $ getFail ("vector length " ++ show n ++ " exceeds maximum")
  -- Every element takes at least one byte on the wire in either encoding
  -- except empty structs; guard against absurd lengths before allocating.
  rem' <- getRemaining
  when (fromIntegral n > rem' && fromIntegral n > (65536 :: Int)) $ getFail "input too short"
  replicateM (fromIntegral n) g

getOptional :: Get a -> Get (Maybe a)
getOptional g = do
  b <- getBool
  if b then Just <$> g else pure Nothing

------------------------------------------------------------------------------
-- CXS

cxsUInt1Max :: Word64
cxsUInt1Max = 62

hdr :: Int -> Word8
hdr 1 = 63
hdr 2 = 127
hdr 4 = 191
hdr _ = 255

leBytes :: Int -> Word64 -> Builder
leBytes n v = mconcat [BB.word8 (fromIntegral (v `shiftR` (8 * i))) | i <- [0 .. n - 1]]

-- | Store an unsigned integer in its canonical CXS form.
cxsPutWord :: Word64 -> Builder
cxsPutWord v
  | v <= cxsUInt1Max = BB.word8 (fromIntegral v)
  | v <= 0xff = BB.word8 (hdr 1) <> leBytes 1 v
  | v <= 0xffff = BB.word8 (hdr 2) <> leBytes 2 v
  | v <= 0xffffffff = BB.word8 (hdr 4) <> leBytes 4 v
  | otherwise = BB.word8 (hdr 8) <> leBytes 8 v

-- | Store a signed integer in its canonical CXS form.
cxsPutInt :: Int64 -> Builder
cxsPutInt v
  | v >= -31 && v <= 31 =
      if v >= 0
        then BB.word8 (fromIntegral v)
        else
          let inv = fromIntegral (negate v) :: Word8
           in BB.word8 ((complement inv .&. 0x1f) .|. 0x20)
  | v >= -128 && v <= 127 = BB.word8 (hdr 1) <> leBytes 1 (fromIntegral v)
  | v >= -32768 && v <= 32767 = BB.word8 (hdr 2) <> leBytes 2 (fromIntegral v)
  | v >= -2147483648 && v <= 2147483647 = BB.word8 (hdr 4) <> leBytes 4 (fromIntegral v)
  | otherwise = BB.word8 (hdr 8) <> leBytes 8 (fromIntegral v)

cxsPutInt32 :: Int32 -> Builder
cxsPutInt32 = cxsPutInt . fromIntegral

cxsPutWord32 :: Word32 -> Builder
cxsPutWord32 = cxsPutWord . fromIntegral

cxsPutInt64 :: Int64 -> Builder
cxsPutInt64 = cxsPutInt

cxsPutWord64 :: Word64 -> Builder
cxsPutWord64 = cxsPutWord

cxsPutBool :: Bool -> Builder
cxsPutBool b = BB.word8 (if b then 1 else 0)

cxsPutFloat :: Float -> Builder
cxsPutFloat = cxsPutWord32 . castFloatToWord32

cxsPutDouble :: Double -> Builder
cxsPutDouble = cxsPutWord64 . castDoubleToWord64

cxsPutFixedOpaque :: Int -> B.ByteString -> Builder
cxsPutFixedOpaque n bs
  | B.length bs /= n = error ("cxsPutFixedOpaque: expected " ++ show n ++ " bytes")
  | otherwise = BB.byteString bs

cxsPutVarOpaque :: Word32 -> B.ByteString -> Builder
cxsPutVarOpaque _ bs = cxsPutWord32 (fromIntegral (B.length bs)) <> BB.byteString bs

cxsPutFixedArray :: Int -> (a -> Builder) -> [a] -> Builder
cxsPutFixedArray _ p = foldMap p

cxsPutVarArray :: Word32 -> (a -> Builder) -> [a] -> Builder
cxsPutVarArray _ p xs = cxsPutWord32 (fromIntegral (length xs)) <> foldMap p xs

cxsPutOptional :: (a -> Builder) -> Maybe a -> Builder
cxsPutOptional _ Nothing = cxsPutBool False
cxsPutOptional p (Just a) = cxsPutBool True <> p a

-- | Size in bytes (1, 2, 4 or 8) selected by a CXS header byte.
cxsHeaderSize :: Word8 -> Get Int
cxsHeaderSize b
  | b .&. 0x3f /= 63 = getFail "cxs: non-canonical"
  | otherwise = pure (case b `shiftR` 6 of 0 -> 1; 1 -> 2; 2 -> 4; _ -> 8)

-- | Load an unsigned value no wider than @width@ bytes.
cxsGetWordW :: Int -> Get Word64
cxsGetWordW width = do
  b <- getByte
  if fromIntegral b <= cxsUInt1Max
    then pure (fromIntegral b)
    else do
      n <- cxsHeaderSize b
      when (n > width) $ getFail "cxs: non-canonical"
      v <- getLE n
      let lowerBound = case n of
            1 -> cxsUInt1Max
            2 -> 0xff
            4 -> 0xffff
            _ -> 0xffffffff
      when (v <= lowerBound) $ getFail "cxs: non-canonical"
      pure v

signExtend :: Int -> Word64 -> Int64
signExtend n v =
  let bits = 8 * n
   in if bits >= 64 then fromIntegral v else fromIntegral (v `shiftL` (64 - bits)) `shiftR` (64 - bits)

cxsGetIntW :: Int -> Get Int64
cxsGetIntW width = do
  b <- getByte
  if fromIntegral b <= cxsUInt1Max
    then
      if b <= 31
        then pure (fromIntegral b)
        else pure (negate (fromIntegral (complement b .&. 0x1f)))
    else do
      n <- cxsHeaderSize b
      when (n > width) $ getFail "cxs: non-canonical"
      v <- signExtend n <$> getLE n
      let (lo, hi) = case n of
            1 -> (-31, 31)
            2 -> (-128, 127)
            4 -> (-32768, 32767)
            _ -> (-2147483648, 2147483647)
      when (v >= lo && v <= hi) $ getFail "cxs: non-canonical"
      pure v

cxsGetWord32 :: Get Word32
cxsGetWord32 = fromIntegral <$> cxsGetWordW 4

cxsGetWord64 :: Get Word64
cxsGetWord64 = cxsGetWordW 8

cxsGetInt32 :: Get Int32
cxsGetInt32 = fromIntegral <$> cxsGetIntW 4

cxsGetInt64 :: Get Int64
cxsGetInt64 = cxsGetIntW 8

cxsGetBool :: Get Bool
cxsGetBool = do
  v <- cxsGetWordW 1
  case v of
    0 -> pure False
    1 -> pure True
    _ -> getFail "cxs: non-canonical bool"

cxsGetFloat :: Get Float
cxsGetFloat = castWord32ToFloat <$> cxsGetWord32

cxsGetDouble :: Get Double
cxsGetDouble = castWord64ToDouble <$> cxsGetWord64

cxsGetFixedOpaque :: Int -> Get B.ByteString
cxsGetFixedOpaque = takeBytes

cxsGetVarOpaque :: Word32 -> Get B.ByteString
cxsGetVarOpaque bound = do
  n <- cxsGetWord32
  when (n > bound) $ getFail "cxs: length exceeded"
  takeBytes (fromIntegral n)

cxsGetFixedArray :: Int -> Get a -> Get [a]
cxsGetFixedArray = replicateM

cxsGetVarArray :: Word32 -> Get a -> Get [a]
cxsGetVarArray bound g = do
  n <- cxsGetWord32
  when (n > bound) $ getFail "cxs: length exceeded"
  rem' <- getRemaining
  when (fromIntegral n > rem' && fromIntegral n > (65536 :: Int)) $ getFail "cxs: truncated"
  replicateM (fromIntegral n) g

cxsGetOptional :: Get a -> Get (Maybe a)
cxsGetOptional g = do
  b <- cxsGetBool
  if b then Just <$> g else pure Nothing

------------------------------------------------------------------------------
-- Instances for primitive types

instance XdrType Int32 where
  xdrPut = putInt32
  xdrGet = getInt32
  cxsPut = cxsPutInt32
  cxsGet = cxsGetInt32

instance XdrType Word32 where
  xdrPut = putWord32
  xdrGet = getWord32
  cxsPut = cxsPutWord32
  cxsGet = cxsGetWord32

instance XdrType Int64 where
  xdrPut = putInt64
  xdrGet = getInt64
  cxsPut = cxsPutInt64
  cxsGet = cxsGetInt64

instance XdrType Word64 where
  xdrPut = putWord64
  xdrGet = getWord64
  cxsPut = cxsPutWord64
  cxsGet = cxsGetWord64

instance XdrType Bool where
  xdrPut = putBool
  xdrGet = getBool
  cxsPut = cxsPutBool
  cxsGet = cxsGetBool

instance XdrType Float where
  xdrPut = putFloat
  xdrGet = getFloat
  cxsPut = cxsPutFloat
  cxsGet = cxsGetFloat

instance XdrType Double where
  xdrPut = putDouble
  xdrGet = getDouble
  cxsPut = cxsPutDouble
  cxsGet = cxsGetDouble

instance XdrType () where
  xdrPut _ = mempty
  xdrGet = pure ()
  cxsPut _ = mempty
  cxsGet = pure ()
