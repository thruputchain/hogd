-- | ML-DSA-65 (FIPS 204) signature verification: a port of
-- @hogd/crypto/mldsa.h@.
--
-- The implementation is mldsa-native (@cbits/mldsa-native@, copied from the
-- @external/mldsa-native@ submodule), compiled by @cbits/hogd_mldsa65_verify.c@
-- with the configuration of hogd's @mldsa_native_verify@ library: parameter
-- set 65, namespace prefix @hogd_mldsa65@, key generation, signing and the
-- randomized API compiled out, portable C only.
module Hogd.Crypto.MlDsa
  ( kMlDsa65PublicKeyBytes
  , kMlDsa65SignatureBytes
  , kMlDsaMaxContextBytes
  , mldsa65Verify
  , mldsa65VerifyCtx
  ) where

import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import qualified Data.ByteString.Unsafe as BU
import Data.Word (Word8)
import Foreign.C.Types (CInt (..), CSize (..))
import Foreign.Ptr (Ptr, castPtr)
import Hogd.Xdr.Types (MlDsa65PublicKey, MlDsa65Signature)
import System.IO.Unsafe (unsafeDupablePerformIO)

-- | Size in bytes of an ML-DSA-65 public key (FIPS 204, Table 2).
kMlDsa65PublicKeyBytes :: Int
kMlDsa65PublicKeyBytes = 1952

-- | Size in bytes of an ML-DSA-65 signature (FIPS 204, Table 2).
kMlDsa65SignatureBytes :: Int
kMlDsa65SignatureBytes = 3309

-- | The length of the longest context string ML-DSA accepts.
kMlDsaMaxContextBytes :: Int
kMlDsaMaxContextBytes = 255

foreign import ccall safe "hogd_mldsa65_verify"
  c_hogd_mldsa65_verify
    :: Ptr Word8 -> Ptr Word8 -> CSize -> Ptr Word8 -> CSize -> Ptr Word8 -> IO CInt

-- | Whether sig is a valid ML-DSA-65 signature on msg under the public key
-- with an empty context string: pure ML-DSA, as transaction authentication
-- and the VerifySignature syscall use (C++ @mldsa65_verify(pk, sig, msg)@).
mldsa65Verify :: MlDsa65PublicKey -> MlDsa65Signature -> ByteString -> Bool
mldsa65Verify pk sig msg = mldsa65VerifyCtx pk sig msg B.empty

-- | ML-DSA.Verify (FIPS 204, Algorithm 3) with the context string ctx
-- (C++ @mldsa65_verify(pk, sig, msg, ctx)@). A ctx longer than
-- 'kMlDsaMaxContextBytes' never verifies. Malformed signatures are rejected
-- like any other invalid one. A key or signature of the wrong length, which
-- the C++ fixed-size types cannot express, never verifies.
mldsa65VerifyCtx :: MlDsa65PublicKey -> MlDsa65Signature -> ByteString -> ByteString -> Bool
mldsa65VerifyCtx pk sig msg ctx
  | B.length pk /= kMlDsa65PublicKeyBytes = False
  | B.length sig /= kMlDsa65SignatureBytes = False
  -- mldsa-native rejects an over-long context string too, but the limit is
  -- FIPS 204's, so enforce it here rather than rely on that.
  | B.length ctx > kMlDsaMaxContextBytes = False
  | otherwise = unsafeDupablePerformIO $
      BU.unsafeUseAsCString sig $ \psig ->
        BU.unsafeUseAsCStringLen msg $ \(pmsg, msgLen) ->
          BU.unsafeUseAsCStringLen ctx $ \(pctx, ctxLen) ->
            BU.unsafeUseAsCString pk $ \ppk -> do
              r <-
                c_hogd_mldsa65_verify
                  (castPtr psig)
                  (castPtr pmsg)
                  (fromIntegral msgLen)
                  (castPtr pctx)
                  (fromIntegral ctxLen)
                  (castPtr ppk)
              pure (r == 0)
