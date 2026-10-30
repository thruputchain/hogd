-- | Ed25519 as hogd does it: a port of the @ed25519_*@ functions of
-- @hogd/crypto/crypto.h@ and the Rust they call (@hogd/rust/crypto/src/ed25519.rs@).
--
-- Despite the @ed25519_dalek_*@ names of the Rust FFI functions, hogd's
-- Ed25519 is the @ed25519-zebra@ crate (4.x), so verification follows
-- ZIP 215 (<https://zips.z.cash/zip-0215>), not ed25519-dalek's @verify@ or
-- @verify_strict@:
--
--  * the public key A and the signature's R are decoded without canonicity
--    checks: a y coordinate in [p, 2^255) is taken mod p, and x = 0 with the
--    sign bit set is accepted. Only an encoding with no curve point (y^2 - 1
--    over d y^2 + 1 not a square) is rejected;
--  * small-order and mixed-order A and R are accepted;
--  * S must be canonical (S < l), otherwise the signature is rejected;
--  * k = SHA-512(R_bytes || A_bytes || M) uses the bytes as given, never
--    re-encoded points;
--  * the equation is cofactored: [8](R - ([S]B - [k]A)) == identity.
--
-- The group arithmetic is crypton's "Crypto.ECC.Edwards25519" (ed25519-donna),
-- whose point decoding accepts exactly the encodings curve25519-dalek's
-- @CompressedEdwardsY::decompress@ does; the checks above are done here. Key
-- generation and signing are RFC 8032, as in ed25519-zebra, and use crypton's
-- "Crypto.PubKey.Ed25519".
module Hogd.Crypto.Ed25519
  ( -- * Sizes
    ed25519PublicKeyBytes
  , ed25519SignatureBytes
  , ed25519SecretKeyBytes
  , ed25519SeedBytes
    -- * Verification
  , Ed25519Result (..)
  , ed25519VerifyResult
  , ed25519Verify
  , ed25519VerifyBatch
    -- * Keys and signing
  , ed25519Keygen
  , ed25519KeygenFromSeed
  , ed25519Sign
  ) where

import Crypto.ECC.Edwards25519
  ( Point
  , pointAdd
  , pointDecode
  , pointEncode
  , pointMulByCofactor
  , pointNegate
  , pointsMulVarTime
  , scalarDecodeLong
  )
import Crypto.Error (CryptoFailable (..))
import Crypto.Hash (Digest, SHA512, hashFinalize, hashInit, hashUpdates)
import qualified Crypto.PubKey.Ed25519 as Ed
import qualified Data.ByteArray as BA
import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import Hogd.Xdr.Types (Ed25519PublicKey, Ed25519SecretKey, Ed25519Signature)
import Crypto.Random (getRandomBytes)

ed25519PublicKeyBytes :: Int
ed25519PublicKeyBytes = 32

ed25519SignatureBytes :: Int
ed25519SignatureBytes = 64

-- | An 'Ed25519SecretKey' is the public key followed by the 32-byte seed
-- (@ED25519_SECRET_KEYPAIR_SIZE@).
ed25519SecretKeyBytes :: Int
ed25519SecretKeyBytes = 64

ed25519SeedBytes :: Int
ed25519SeedBytes = 32

-- | The Rust FFI's result codes (@Ed25519Result@). Verification produces
-- 'Ed25519Success', 'Ed25519InvalidPublicKey' (A does not decode) or
-- 'Ed25519VerificationFailed'; 'Ed25519InvalidInputLength' is for byte
-- strings of the wrong size, which the C++ fixed-size types cannot express.
data Ed25519Result
  = Ed25519Success
  | Ed25519InvalidPublicKey
  | Ed25519InvalidSignature
  | Ed25519VerificationFailed
  | Ed25519InvalidInputLength
  deriving (Eq, Ord, Show, Enum, Bounded)

-- | ed25519-zebra's @VerificationKey::try_from(bytes)@ then @verify@.
ed25519VerifyResult :: Ed25519PublicKey -> Ed25519Signature -> ByteString -> Ed25519Result
ed25519VerifyResult pk sig msg
  | B.length pk /= ed25519PublicKeyBytes || B.length sig /= ed25519SignatureBytes =
      Ed25519InvalidInputLength
  | otherwise = case pointDecode pk of
      CryptoFailed _ -> Ed25519InvalidPublicKey
      CryptoPassed a
        | verifyWithKey pk a sig msg -> Ed25519Success
        | otherwise -> Ed25519VerificationFailed

-- | Whether sig is a valid signature on msg under pk (C++ @ed25519_verify@).
-- Byte strings of the wrong length never verify.
ed25519Verify :: Ed25519PublicKey -> Ed25519Signature -> ByteString -> Bool
ed25519Verify pk sig msg = ed25519VerifyResult pk sig msg == Ed25519Success

-- | C++ @ed25519_verify_batch@: false if the three lists differ in length,
-- true for an empty batch, and otherwise whether every signature verifies.
-- ed25519-zebra's batch verifier checks the cofactored equation under random
-- coefficients, so it accepts exactly when every member verifies on its own
-- (up to a negligible probability).
ed25519VerifyBatch :: [Ed25519PublicKey] -> [Ed25519Signature] -> [ByteString] -> Bool
ed25519VerifyBatch pks sigs msgs
  | length pks /= length sigs || length pks /= length msgs = False
  | otherwise = and (zipWith3 ed25519Verify pks sigs msgs)

verifyWithKey :: ByteString -> Point -> ByteString -> ByteString -> Bool
verifyWithKey aBytes a sig msg =
  let (rBytes, sBytes) = B.splitAt 32 sig
   in isCanonicalScalar sBytes && case pointDecode rBytes of
        CryptoFailed _ -> False
        CryptoPassed r ->
          let k = scalar64 (sha512 [rBytes, aBytes, msg])
              s = scalar64 sBytes
              -- R' = [s]B - [k]A, as ed25519-zebra's
              -- vartime_double_scalar_mul_basepoint(k, -A, s).
              r' = pointsMulVarTime s k (pointNegate a)
              d = pointMulByCofactor (pointAdd r (pointNegate r'))
           in (pointEncode d :: ByteString) == identityEncoding
  where
    scalar64 bs = case scalarDecodeLong bs of
      CryptoPassed x -> x
      CryptoFailed e -> error ("ed25519: scalar decode: " ++ show e)

-- | The encoding of the identity point (x = 0, y = 1).
identityEncoding :: ByteString
identityEncoding = B.cons 1 (B.replicate 31 0)

-- | Whether 32 little-endian bytes are a canonical scalar, less than the
-- group order l = 2^252 + 27742317777372353535851937790883648493
-- (curve25519-dalek's @Scalar::from_canonical_bytes@).
isCanonicalScalar :: ByteString -> Bool
isCanonicalScalar bs = B.length bs == 32 && leInteger bs < groupOrder

groupOrder :: Integer
groupOrder = 2 ^ (252 :: Int) + 27742317777372353535851937790883648493

leInteger :: ByteString -> Integer
leInteger = B.foldr' (\b acc -> acc * 256 + fromIntegral b) 0

sha512 :: [ByteString] -> ByteString
sha512 parts = BA.convert (hashFinalize (hashUpdates hashInit parts) :: Digest SHA512)

-- | A fresh random key pair (C++ @ed25519_keygen@).
ed25519Keygen :: IO (Ed25519PublicKey, Ed25519SecretKey)
ed25519Keygen = ed25519KeygenFromSeed <$> getRandomBytes ed25519SeedBytes

-- | The key pair for a 32-byte seed (C++ @ed25519_keygen_from_seed@, Rust
-- @ed25519_dalek_keygen@): the RFC 8032 public key, and the secret key
-- public key || seed. Calls 'error' if the seed is not 32 bytes long (a
-- @std::span<const std::byte, 32>@ in C++).
ed25519KeygenFromSeed :: ByteString -> (Ed25519PublicKey, Ed25519SecretKey)
ed25519KeygenFromSeed seed = case Ed.secretKey seed of
  CryptoPassed sk ->
    let pk = BA.convert (Ed.toPublic sk)
     in (pk, pk <> seed)
  CryptoFailed _ -> error "ed25519KeygenFromSeed: the seed must be 32 bytes"

-- | Sign msg (C++ @ed25519_sign@, Rust @ed25519_dalek_sign@): deterministic
-- RFC 8032 signing with the seed in the secret key's second half. Nothing if
-- the secret key's first half is not the seed's public key (or the secret key
-- is not 64 bytes long).
ed25519Sign :: Ed25519SecretKey -> ByteString -> Maybe Ed25519Signature
ed25519Sign keypair msg
  | B.length keypair /= ed25519SecretKeyBytes = Nothing
  | otherwise = case Ed.secretKey seed of
      CryptoFailed _ -> Nothing
      CryptoPassed sk ->
        let pk = Ed.toPublic sk
         in if BA.convert pk /= expectedPk
              then Nothing
              else Just (BA.convert (Ed.sign sk pk msg))
  where
    (expectedPk, seed) = B.splitAt 32 keypair
