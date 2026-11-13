-- | ML-DSA-65 (FIPS 204) key generation and signing, for tests: a port of
-- @hogd/crypto/mldsa_signer.h@.
--
-- hogd only verifies ML-DSA signatures ("Hogd.Crypto.MlDsa"); this lets tests
-- make them. It is a second build of mldsa-native
-- (@cbits/hogd_mldsa65_signer.c@) with the configuration of hogd's
-- @mldsa_native_signer@ library: parameter set 65, namespace prefix
-- @hogd_mldsa65_signer@, core API only, verification and the randomized API
-- compiled out. Randomness is supplied by the caller.
module Hogd.Crypto.MlDsaSigner
  ( kMlDsa65SecretKeyBytes
  , kMlDsaSeedBytes
  , kMlDsaRndBytes
  , MlDsaSeed
  , MlDsaRnd
  , MlDsa65SigningKey
  , mldsa65Generate
  , mldsa65FromSeed
  , mldsa65PublicKey
  , mldsa65Sign
  , mldsa65SignWithRnd
  ) where

import Control.Monad (when)
import Crypto.Random (getRandomBytes)
import Data.ByteArray (ScrubbedBytes)
import qualified Data.ByteArray as BA
import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import qualified Data.ByteString.Unsafe as BU
import Data.Word (Word8)
import Foreign.C.Types (CInt (..), CSize (..))
import Foreign.Ptr (Ptr, castPtr)
import Hogd.Crypto.MlDsa (kMlDsa65PublicKeyBytes, kMlDsa65SignatureBytes, kMlDsaMaxContextBytes)
import Hogd.Xdr.Types (MlDsa65PublicKey, MlDsa65Signature)
import System.IO.Unsafe (unsafeDupablePerformIO)

-- | Size in bytes of an ML-DSA-65 secret key (FIPS 204, Table 2).
kMlDsa65SecretKeyBytes :: Int
kMlDsa65SecretKeyBytes = 4032

kMlDsaSeedBytes :: Int
kMlDsaSeedBytes = 32

kMlDsaRndBytes :: Int
kMlDsaRndBytes = 32

-- | The 32-byte seed key generation derives a key pair from (xi in FIPS 204,
-- Algorithm 6).
type MlDsaSeed = ByteString

-- | The 32-byte per-signature randomness of ML-DSA.Sign (rnd in FIPS 204,
-- Algorithm 2).
type MlDsaRnd = ByteString

-- | An ML-DSA-65 key pair. The secret key is held in 'ScrubbedBytes', which
-- are zeroed when collected (the C++ class zeroes it when destroyed).
data MlDsa65SigningKey = MlDsa65SigningKey
  { skPublic :: !MlDsa65PublicKey
  , skSecret :: !ScrubbedBytes
  }

foreign import ccall safe "hogd_mldsa65_signer_keypair_internal"
  c_keypair_internal :: Ptr Word8 -> Ptr Word8 -> Ptr Word8 -> IO CInt

foreign import ccall safe "hogd_mldsa65_signer_signature_internal"
  c_signature_internal
    :: Ptr Word8 -- sig
    -> Ptr Word8 -- m
    -> CSize -- mlen
    -> Ptr Word8 -- pre
    -> CSize -- prelen
    -> Ptr Word8 -- rnd
    -> Ptr Word8 -- sk
    -> CInt -- externalmu
    -> IO CInt

-- | A key pair from a fresh random seed (ML-DSA.KeyGen).
mldsa65Generate :: IO MlDsa65SigningKey
mldsa65Generate = do
  seed <- getRandomBytes kMlDsaSeedBytes :: IO ScrubbedBytes
  pure $! fromSeedBytes seed

-- | The key pair for a seed (ML-DSA.KeyGen_internal), as the ACVP and
-- Wycheproof key generation vectors do. Calls 'error' if the seed is not 32
-- bytes long (a @std::array<uint8_t, 32>@ in C++).
mldsa65FromSeed :: MlDsaSeed -> MlDsa65SigningKey
mldsa65FromSeed seed = fromSeedBytes (BA.convert seed :: ScrubbedBytes)

fromSeedBytes :: ScrubbedBytes -> MlDsa65SigningKey
fromSeedBytes seed
  | BA.length seed /= kMlDsaSeedBytes = error "mldsa65FromSeed: the seed must be 32 bytes"
  | otherwise = unsafeDupablePerformIO $
      BA.withByteArray seed $ \pseed -> do
        ((r, sk), pk) <- BA.allocRet kMlDsa65PublicKeyBytes $ \ppk ->
          BA.allocRet kMlDsa65SecretKeyBytes $ \psk ->
            c_keypair_internal ppk psk pseed
        -- Key generation can only fail in configurations hogd does not build
        -- (custom allocators, the FIPS 140-3 pairwise consistency test).
        when (r /= 0) $ error "mldsa65FromSeed: mldsa-native key generation failed"
        pure (MlDsa65SigningKey pk sk)

mldsa65PublicKey :: MlDsa65SigningKey -> MlDsa65PublicKey
mldsa65PublicKey = skPublic

-- | Sign msg with the context string ctx as ML-DSA.Sign (FIPS 204,
-- Algorithm 2) does, hedged with fresh randomness: signing the same message
-- twice gives two different valid signatures. Nothing if ctx is longer than
-- 'kMlDsaMaxContextBytes'.
mldsa65Sign :: MlDsa65SigningKey -> ByteString -> ByteString -> IO (Maybe MlDsa65Signature)
mldsa65Sign key msg ctx = do
  rnd <- getRandomBytes kMlDsaRndBytes
  pure $! mldsa65SignWithRnd key msg ctx rnd

-- | 'mldsa65Sign' with the per-signature randomness supplied. An all-zero rnd
-- gives deterministic ML-DSA (FIPS 204, Section 3.4). Calls 'error' if rnd is
-- not 32 bytes long.
mldsa65SignWithRnd :: MlDsa65SigningKey -> ByteString -> ByteString -> MlDsaRnd -> Maybe MlDsa65Signature
mldsa65SignWithRnd key msg ctx rnd
  | B.length rnd /= kMlDsaRndBytes = error "mldsa65SignWithRnd: rnd must be 32 bytes"
  | B.length ctx > kMlDsaMaxContextBytes = Nothing
  | otherwise = unsafeDupablePerformIO $
      -- Pure ML-DSA signs the message behind the prefix
      -- 0 || IntegerToBytes(|ctx|, 1) || ctx (FIPS 204, Algorithm 2, line 10).
      let prefix = B.pack [0, fromIntegral (B.length ctx)] <> ctx
       in BU.unsafeUseAsCStringLen msg $ \(pmsg, msgLen) ->
            BU.unsafeUseAsCStringLen prefix $ \(ppre, preLen) ->
              BU.unsafeUseAsCString rnd $ \prnd ->
                BA.withByteArray (skSecret key) $ \psk -> do
                  (r, sig) <- BA.allocRet kMlDsa65SignatureBytes $ \psig ->
                    c_signature_internal
                      psig
                      (castPtr pmsg)
                      (fromIntegral msgLen)
                      (castPtr ppre)
                      (fromIntegral preLen)
                      (castPtr prnd)
                      psk
                      0
                  pure (if r == 0 then Just sig else Nothing)
