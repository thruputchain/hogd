-- | Cryptographic utilities: a port of @hogd/crypto/crypto.h@.
--
-- The executor (@hogd/hog_exec@) uses SHA-256 (one-shot, streaming, and over
-- the XDR encoding of a value), keyed SipHash-2-4 for attacker-keyed hash
-- tables, and Ed25519 (re-exported from "Hogd.Crypto.Ed25519"). ML-DSA-65
-- lives in "Hogd.Crypto.MlDsa" (verification) and "Hogd.Crypto.MlDsaSigner"
-- (key generation and signing, for tests), and the domain-separated hashes
-- of transaction authentication in "Hogd.Crypto.DomainSeparators".
--
-- Not ported, because nothing in hog_exec uses them: @blake3@, @crc32c@,
-- @crc32c_append@ and @lthash16.h@.
module Hogd.Crypto
  ( -- * SHA-256
    SHA256Hasher
  , sha256HasherNew
  , sha256HasherUpdate
  , sha256HasherUpdates
  , sha256HasherFinalize
  , sha256
  , sha256Lazy
  , sha256Xdr
  , sha256HashBytes
    -- * Randomness
  , readRandom
    -- * SipHash-2-4
  , SipHash24Key
  , sipHash24
  , sipHash24DefaultKey
  , sipHash24Default
  , sipHash24Word32
    -- * Ed25519
  , module Hogd.Crypto.Ed25519
  ) where

import Crypto.Hash (Context, Digest, SHA256, hashFinalize, hashInit, hashUpdate, hashUpdates, hashlazy)
import qualified Crypto.Hash as H
import Crypto.Random (getRandomBytes)
import qualified Data.ByteArray as BA
import Data.ByteArray.Hash (SipHash (..), SipKey (..), sipHash)
import Data.Bits (shiftR)
import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import qualified Data.ByteString.Lazy as BL
import Data.Word (Word32, Word64, Word8)
import Hogd.Crypto.Ed25519
import Hogd.Xdr.Types (SHA256Hash)
import System.IO.Unsafe (unsafePerformIO)
import X23.Xdr (XdrType, xdrEncode)

-- | sha256HashBytes is the size of a SHA-256 digest.
sha256HashBytes :: Int
sha256HashBytes = 32

-- | An incremental SHA-256 computation (C++ @SHA256Hasher@).
--
-- Unlike the C++ class, which wraps a mutable OpenSSL context, a hasher is an
-- immutable value: updating returns a new hasher, so copying a stream (the
-- @SHA256Copy@ syscall) is just keeping the old value, and finalizing does
-- not consume it.
newtype SHA256Hasher = SHA256Hasher (Context SHA256)

-- | A hasher that has absorbed nothing.
sha256HasherNew :: SHA256Hasher
sha256HasherNew = SHA256Hasher hashInit

-- | Absorb bytes. Absorbing nothing is a no-op, as in C++.
sha256HasherUpdate :: SHA256Hasher -> ByteString -> SHA256Hasher
sha256HasherUpdate h@(SHA256Hasher ctx) bs
  | B.null bs = h
  | otherwise = SHA256Hasher (hashUpdate ctx bs)

-- | Absorb several byte strings in order.
sha256HasherUpdates :: SHA256Hasher -> [ByteString] -> SHA256Hasher
sha256HasherUpdates (SHA256Hasher ctx) bss = SHA256Hasher (hashUpdates ctx bss)

-- | The digest of everything absorbed so far.
sha256HasherFinalize :: SHA256Hasher -> SHA256Hash
sha256HasherFinalize (SHA256Hasher ctx) = digestBytes (hashFinalize ctx)

-- | SHA-256 of a byte string (C++ @sha256(x23::BufView)@).
sha256 :: ByteString -> SHA256Hash
sha256 = digestBytes . (H.hash :: ByteString -> Digest SHA256)

-- | SHA-256 of a lazy byte string.
sha256Lazy :: BL.ByteString -> SHA256Hash
sha256Lazy = digestBytes . (hashlazy :: BL.ByteString -> Digest SHA256)

-- | SHA-256 of the XDR encoding of a value
-- (C++ @sha256(const T &)@ = @sha256(x23::serialize(value))@).
sha256Xdr :: XdrType a => a -> SHA256Hash
sha256Xdr = sha256Lazy . xdrEncode

digestBytes :: Digest SHA256 -> ByteString
digestBytes = BA.convert

-- | High-quality random bytes suitable for secret keys (C++ @read_random@).
readRandom :: Int -> IO ByteString
readRandom = getRandomBytes

-- | The secret key of SipHash-2-4: the two 64-bit key words.
type SipHash24Key = (Word64, Word64)

-- | SipHash-2-4 of the bytes under key (C++ @SipHash24::operator()@, which is
-- Rust's @siphasher::sip::SipHasher24::new_with_keys(k0, k1)@ fed the bytes
-- with one @write@).
sipHash24 :: SipHash24Key -> ByteString -> Word64
sipHash24 (k0, k1) bs = let SipHash h = sipHash (SipKey k0 k1) bs in h

-- | The process-wide random key (C++ @SipHash24::get_default()@). It differs
-- between processes, so nothing derived from it may reach consensus state.
sipHash24DefaultKey :: SipHash24Key
sipHash24DefaultKey = unsafePerformIO $ do
  bs <- readRandom 16
  pure (le64 (B.take 8 bs), le64 (B.drop 8 bs))
{-# NOINLINE sipHash24DefaultKey #-}

-- | SipHash-2-4 under the process-wide key.
sipHash24Default :: ByteString -> Word64
sipHash24Default = sipHash24 sipHash24DefaultKey

-- | The C++ @SipHash24Hasher@ on a @uint32_t@: the hash of its four native
-- (little-endian) bytes under the process-wide key.
sipHash24Word32 :: Word32 -> Word64
sipHash24Word32 w = sipHash24Default (B.pack [byte 0, byte 8, byte 16, byte 24])
  where
    byte :: Int -> Word8
    byte n = fromIntegral (w `shiftR` n)

le64 :: ByteString -> Word64
le64 = B.foldr' (\b acc -> acc * 256 + fromIntegral b) 0
