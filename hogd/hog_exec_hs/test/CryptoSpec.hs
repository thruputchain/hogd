{-# LANGUAGE DataKinds #-}

module CryptoSpec (spec) where

import Control.Monad (forM, forM_, unless)
import Crypto.Hash (Context, Digest, SHAKE128, hashFinalize, hashInit, hashUpdates)
import qualified Crypto.Hash as H
import Crypto.ECC.Edwards25519 (pointDecode, pointEncode)
import Crypto.Error (CryptoFailable (..))
import qualified Data.ByteArray as BA
import Data.Bits (clearBit, shiftL, shiftR, testBit, xor, (.&.), (.|.))
import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import Data.ByteString (ByteString)
import Data.Char (digitToInt, intToDigit)
import Data.Maybe (fromMaybe, isJust, isNothing)
import Data.Word (Word64, Word8)
import qualified Ed25519EdgeCases as EC
import Hogd.Crypto
import Hogd.Crypto.DomainSeparators
import Hogd.Crypto.MlDsa
import Hogd.Crypto.MlDsaSigner
import Hogd.Xdr.AuthClaim (PubkeyAccountPreimage (..))
import Hogd.Xdr.Contract (ContractDeploymentHashPreimage (..))
import Hogd.Xdr.Preimage (DomainSeparator (..))
import Hogd.Xdr.Types (ExtensionPoint (..), PublicKey (..))
import qualified MiniJson as J
import System.Directory (doesFileExist)
import System.Environment (lookupEnv)
import Test.Hspec
import Test.QuickCheck hiding ((.&.))
import X23.Xdr (cxsDecode, cxsEncode)

spec :: Spec
spec = do
  sha256Spec
  sipHashSpec
  domainSeparatorSpec
  ed25519Spec
  mldsaSpec

------------------------------------------------------------------------------
-- Helpers

unhex :: String -> ByteString
unhex s
  | odd (length s) = error ("odd-length hex: " ++ s)
  | otherwise = B.pack (go s)
  where
    go (a : b : r) = fromIntegral (digitToInt a * 16 + digitToInt b) : go r
    go _ = []

tohex :: ByteString -> String
tohex = concatMap (\w -> [intToDigit (fromIntegral (w `shiftR` 4)), intToDigit (fromIntegral (w .&. 15))]) . B.unpack

-- | Flip bit n mod 8 of byte n.
flipBitAt :: Int -> ByteString -> ByteString
flipBitAt i bs =
  let (a, b) = B.splitAt i bs
   in a <> B.cons (B.head b `xor` (1 `shiftL` (i `mod` 8))) (B.tail b)

wycheproofFile :: String -> IO (Maybe FilePath)
wycheproofFile name = do
  dir <- fromMaybe "../../external/wycheproof/testvectors_v1" <$> lookupEnv "HOGD_WYCHEPROOF_DIR"
  let path = dir ++ "/" ++ name
  ok <- doesFileExist path
  pure (if ok then Just path else Nothing)

withWycheproof :: String -> (J.Value -> Expectation) -> Expectation
withWycheproof name k = do
  mpath <- wycheproofFile name
  case mpath of
    Nothing ->
      pendingWith
        ( name
            ++ " not found: check out external/wycheproof (git submodule update --init external/wycheproof) or set HOGD_WYCHEPROOF_DIR"
        )
    Just path -> do
      -- Bytes as Latin-1 characters: only comments hold non-ASCII text.
      text <- BC.unpack <$> B.readFile path
      case J.parseJson text of
        Left e -> expectationFailure (path ++ ": " ++ e)
        Right doc -> k doc

hasFlag :: String -> J.Value -> Bool
hasFlag f t = maybe False ((f `elem`) . J.strs) (J.fieldMaybe "flags" t)

------------------------------------------------------------------------------
-- SHA-256

sha256Spec :: Spec
sha256Spec = describe "SHA-256" $ do
  it "matches the FIPS 180-2 known answers" $ do
    tohex (sha256 "") `shouldBe` "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    tohex (sha256 "abc") `shouldBe` "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    tohex (sha256 "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
      `shouldBe` "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    tohex (sha256 (BC.replicate 1000000 'a'))
      `shouldBe` "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"

  it "distinguishes inputs (crypto_test.cc SHA256.BasicUsage)" $ do
    sha256 (B.pack [0, 1, 2, 3]) `shouldBe` sha256 (B.pack [0, 1, 2, 3])
    sha256 (B.pack [0, 1, 2, 3]) `shouldNotBe` sha256 (B.pack [3, 2, 1, 0])

  it "streams to the one-shot digest (IncrementalMatchesOneShot)" $ do
    let msg = "incremental sha256 coverage" :: ByteString
    sha256HasherFinalize sha256HasherNew `shouldBe` sha256 B.empty
    sha256HasherFinalize (sha256HasherUpdate sha256HasherNew msg) `shouldBe` sha256 msg
    let chunks = [B.take 5 msg, B.take 7 (B.drop 5 msg), B.drop 12 msg]
    sha256HasherFinalize (foldl sha256HasherUpdate sha256HasherNew chunks) `shouldBe` sha256 msg
    sha256HasherFinalize (sha256HasherUpdates sha256HasherNew chunks) `shouldBe` sha256 msg

  it "ignores empty updates anywhere (IncrementalConcatEquivalence)" $ do
    let a = B.pack [0x10, 0x20, 0x30]
        b = B.pack [0x40, 0x50, 0x60, 0x70]
        ab = a <> b
        run = sha256HasherFinalize . foldl sha256HasherUpdate sha256HasherNew
    run [a, b] `shouldBe` sha256 ab
    run [B.empty, ab] `shouldBe` sha256 ab
    run [ab, B.empty] `shouldBe` sha256 ab
    run [a, B.empty, b] `shouldBe` sha256 ab

  it "streams any split of any input to the one-shot digest" $
    property $ \(ws :: [[Word8]]) ->
      let parts = map B.pack ws
       in sha256HasherFinalize (foldl sha256HasherUpdate sha256HasherNew parts) === sha256 (B.concat parts)

  it "copies a stream: the copy and the original continue independently" $ do
    -- SHA256Copy then diverging SHA256Update calls on src and dst, across the
    -- 64-byte block boundary.
    let prefix = BC.replicate 100 'p'
        src = sha256HasherUpdate sha256HasherNew prefix
        dst = src
        src' = sha256HasherUpdate src "source tail"
        dst' = sha256HasherUpdate dst (BC.replicate 30 'd')
    sha256HasherFinalize src' `shouldBe` sha256 (prefix <> "source tail")
    sha256HasherFinalize dst' `shouldBe` sha256 (prefix <> BC.replicate 30 'd')
    -- Finalizing does not disturb the stream it was taken from.
    sha256HasherFinalize src `shouldBe` sha256 prefix
    sha256HasherFinalize (sha256HasherUpdate src "x") `shouldBe` sha256 (prefix <> "x")

  it "hashes the XDR encoding of a value" $ do
    -- sha256(uint64_t 0), as crypto_test.cc's FFIBug test computes.
    sha256Xdr (0 :: Word64) `shouldBe` sha256 (B.replicate 8 0)
    sha256Xdr (0x0102030405060708 :: Word64) `shouldBe` sha256 (B.pack [1 .. 8])
    let preimage =
          ContractDeploymentHashPreimage
            { deployer_address = B.replicate 32 1
            , nonce = B.replicate 32 2
            , contract_hash = B.replicate 32 3
            }
    sha256Xdr preimage `shouldBe` sha256 (B.concat [B.replicate 32 1, B.replicate 32 2, B.replicate 32 3])

------------------------------------------------------------------------------
-- SipHash-2-4

sipHashSpec :: Spec
sipHashSpec = describe "SipHash-2-4" $ do
  it "matches the reference vectors (key 00..0f, message 00..len-1)" $ do
    let key = (0x0706050403020100, 0x0f0e0d0c0b0a0908)
    sipHash24 key B.empty `shouldBe` 0x726fdb47dd0e0e31
    sipHash24 key (B.pack [0 .. 14]) `shouldBe` 0xa129ca6149be45e5
    sipHash24 key (B.pack [0 .. 62]) `shouldBe` 0x958a324ceb064572

  it "depends on the key and the input (Siphash24.BasicUsage)" $ do
    let a = B.pack [0, 1, 2, 3]
        b = B.pack [3, 2, 1, 0]
    sipHash24Default a `shouldBe` sipHash24Default a
    sipHash24Default a `shouldNotBe` sipHash24Default b
    sipHash24 (1, 2) a `shouldNotBe` sipHash24 (3, 4) a
    sipHash24Word32 7 `shouldBe` sipHash24Default (B.pack [7, 0, 0, 0])

------------------------------------------------------------------------------
-- Domain separators

domainSeparatorSpec :: Spec
domainSeparatorSpec = describe "domain separators" $ do
  it "encodes PubkeyAccount as crypto_test.cc pins" $
    B.unpack kPubkeyAuthDomainSeparatorCxs
      `shouldBe` [0x00, 0x0D, 0x50, 0x75, 0x62, 0x6B, 0x65, 0x79, 0x41, 0x63, 0x63, 0x6F, 0x75, 0x6E, 0x74]

  it "encodes PublicKey and Transaction the same way" $ do
    B.unpack kPublicKeyHashDomainSeparatorCxs
      `shouldBe` [0x00, 0x09, 0x50, 0x75, 0x62, 0x6C, 0x69, 0x63, 0x4B, 0x65, 0x79]
    B.unpack kTransactionSignatureDomainSeparatorCxs
      `shouldBe` [0x00, 0x0B, 0x54, 0x72, 0x61, 0x6E, 0x73, 0x61, 0x63, 0x74, 0x69, 0x6F, 0x6E]

  it "is the CXS encoding of a DomainSeparator value" $ do
    cxsDecode kPublicKeyHashDomainSeparatorCxs
      `shouldBe` Right (DomainSeparator {ext = ExtensionPoint_0, domain_separator = "PublicKey"})
    -- A name of 63 bytes or more needs a multi-byte CXS length.
    domainSeparatorCxs (BC.replicate 63 'x') `shouldBe` B.pack [0x00, 0x3F, 0x3F] <> BC.replicate 63 'x'

  let edPk = B.pack [0 .. 31]
      edKey = PublicKey_ed25519 edPk
      mlPk = B.pack [fromIntegral (i * 7) | i <- [0 .. 1951 :: Int]]
      mlKey = PublicKey_mldsa65 mlPk
      edHash = unhex "37c5d161f6a5687f0a6e3466492a4a80d120b780af39a54fe198b48bd2888a56"

  it "hashes a public key with its scheme" $ do
    publicKeyHash edKey `shouldBe` Just edHash
    publicKeyHash edKey `shouldBe` Just (sha256 (kPublicKeyHashDomainSeparatorCxs <> B.singleton 0 <> edPk))
    publicKeyHash mlKey
      `shouldBe` Just (unhex "3b7e64a5ab0e0aac8e7eda4dff4affd714e1e8c515d576f42535a932f4b2f656")
    publicKeyHash mlKey `shouldBe` Just (sha256 (kPublicKeyHashDomainSeparatorCxs <> B.singleton 1 <> mlPk))

  it "rejects keys that cannot be serialized" $ do
    publicKeyHash (PublicKey_ed25519 (B.replicate 31 0)) `shouldBe` Nothing
    publicKeyHash (PublicKey_mldsa65 (B.replicate 32 0)) `shouldBe` Nothing
    pubkeyAuthAddress (PubkeyAccountPreimage {pubkey_hash = B.replicate 31 0, nonce = 0}) `shouldBe` Nothing

  it "derives standalone-pubkey account addresses" $ do
    let pre7 = makePubkeyAccountPreimage edKey 7
    pre7 `shouldBe` PubkeyAccountPreimage {pubkey_hash = edHash, nonce = 7}
    pubkeyAuthAddress pre7
      `shouldBe` Just (unhex "e2829dbf0ca50f0264c766603658c7b14b710c51c1421971f9534c39d1f04e42")
    pubkeyAuthAddress pre7 `shouldBe` Just (sha256 (kPubkeyAuthDomainSeparatorCxs <> edHash <> B.singleton 7))
    pubkeyAuthAddress (makePubkeyAccountPreimage edKey 300)
      `shouldBe` Just (unhex "f473d06c8f71668c17e608b7b6057c47eff8f638f56586f6ddd6abd9b0b6cd80")
    pubkeyAuthAddress (makePubkeyAccountPreimage mlKey 0)
      `shouldBe` Just (unhex "9ae11f265efa99efceeee3757145ea1dc86c0ccb96cb1c7b01b3724255315b81")

  it "names different accounts for the same bytes under different schemes" $ do
    let mlSame = PublicKey_mldsa65 (edPk <> B.replicate (1952 - 32) 0)
    publicKeyHash edKey `shouldNotBe` publicKeyHash mlSame
    pubkeyAuthAddress (makePubkeyAccountPreimage edKey 7)
      `shouldNotBe` pubkeyAuthAddress (makePubkeyAccountPreimage mlSame 7)

  it "serializes the preimage with CXS" $
    cxsEncode (PubkeyAccountPreimage {pubkey_hash = edHash, nonce = 300}) `shouldBe` edHash <> B.pack [0x7f, 0x2c, 0x01]

------------------------------------------------------------------------------
-- Ed25519

-- Reference edwards25519 point decoding with Integer arithmetic, as
-- curve25519-dalek's CompressedEdwardsY::decompress: y is the low 255 bits
-- taken mod p, accepted iff (y^2 - 1) / (d y^2 + 1) is a square, and x is the
-- root whose parity is the sign bit (x = 0 for either sign bit). Returns the
-- canonical encoding of the decoded point.
refDecode :: ByteString -> Maybe ByteString
refDecode bs =
  let n = leInt bs
      sign = testBit n 255
      y = clearBit n 255 `mod` p
      u = (y * y - 1) `mod` p
      v = (d * y * y + 1) `mod` p
      x2 = u * powMod v (p - 2) `mod` p
      r0 = powMod x2 ((p + 3) `div` 8)
      r = if r0 * r0 `mod` p == x2 then r0 else r0 * sqrtM1 `mod` p
      x = if odd r == sign then r else (p - r) `mod` p
   in if r * r `mod` p /= x2
        then Nothing
        else Just (leBytes (y .|. (if odd x then 1 `shiftL` 255 else 0)))
  where
    p = 2 ^ (255 :: Int) - 19 :: Integer
    d = (-121665) * powMod 121666 (p - 2) `mod` p
    sqrtM1 = powMod 2 ((p - 1) `div` 4)
    powMod :: Integer -> Integer -> Integer
    powMod b e
      | e == 0 = 1
      | even e = let h = powMod b (e `div` 2) in h * h `mod` p
      | otherwise = b * powMod b (e - 1) `mod` p
    leInt = B.foldr' (\w acc -> acc * 256 + fromIntegral w) 0
    leBytes :: Integer -> ByteString
    leBytes v = B.pack [fromIntegral (v `shiftR` (8 * i)) | i <- [0 .. 31]]

-- | 32 little-endian bytes of an integer.
encLE :: Integer -> ByteString
encLE v = B.pack [fromIntegral (v `shiftR` (8 * i)) | i <- [0 .. 31 :: Int]]

groupOrderL :: Integer
groupOrderL = 2 ^ (252 :: Int) + 27742317777372353535851937790883648493

ed25519Spec :: Spec
ed25519Spec = describe "Ed25519" $ do
  -- RFC 8032, section 7.1, tests 1-3.
  let rfc =
        [ ( "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
          , "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
          , ""
          , "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
          )
        , ( "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"
          , "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"
          , "72"
          , "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
          )
        , ( "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7"
          , "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025"
          , "af82"
          , "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
          )
        ]

  it "derives RFC 8032 keys from seeds, as public key || seed" $
    forM_ rfc $ \(seed, pk, _, _) ->
      ed25519KeygenFromSeed (unhex seed) `shouldBe` (unhex pk, unhex pk <> unhex seed)

  it "signs as RFC 8032 and verifies its signatures" $
    forM_ rfc $ \(seed, pk, msg, sig) -> do
      let (_, sk) = ed25519KeygenFromSeed (unhex seed)
      ed25519Sign sk (unhex msg) `shouldBe` Just (unhex sig)
      ed25519Verify (unhex pk) (unhex sig) (unhex msg) `shouldBe` True
      ed25519VerifyResult (unhex pk) (unhex sig) (unhex msg) `shouldBe` Ed25519Success
      ed25519Verify (unhex pk) (unhex sig) (unhex msg <> "x") `shouldBe` False

  it "signs and verifies with fresh keys (Ed25519.BasicUsage, FFIBug)" $ do
    (pk, sk) <- ed25519Keygen
    B.length pk `shouldBe` 32
    B.length sk `shouldBe` 64
    Just sig <- pure (ed25519Sign sk "test message")
    ed25519Verify pk sig "test message" `shouldBe` True
    ed25519Verify pk sig "wrong message" `shouldBe` False
    let msg = sha256Xdr (0 :: Word64)
    Just sig2 <- pure (ed25519Sign sk msg)
    ed25519Verify pk sig2 msg `shouldBe` True
    (pk', sk') <- ed25519Keygen
    pk' `shouldNotBe` pk
    sk' `shouldNotBe` sk

  it "refuses to sign with a mismatched key pair (SignReturnsNulloptForMismatchedKeypair)" $ do
    (pk, sk) <- ed25519Keygen
    let msg = "message signed with mismatched keypair" :: ByteString
    fmap (\s -> ed25519Verify pk s msg) (ed25519Sign sk msg) `shouldBe` Just True
    ed25519Sign (flipBitAt 0 sk) msg `shouldBe` Nothing
    ed25519Sign (B.take 63 sk) msg `shouldBe` Nothing

  it "verifies batches (BatchUsage, BatchUsageEmpty)" $ do
    keys <- forM [1 :: Int, 2, 3] (const ed25519Keygen)
    let msgs = ["test message", "another message", "yet another message"]
        sigs = [s | ((_, sk), m) <- zip keys msgs, Just s <- [ed25519Sign sk m]]
        pks = map fst keys
    ed25519VerifyBatch pks sigs msgs `shouldBe` True
    ed25519VerifyBatch [] [] [] `shouldBe` True
    ed25519VerifyBatch pks sigs (reverse msgs) `shouldBe` False
    ed25519VerifyBatch pks (take 2 sigs) msgs `shouldBe` False
    ed25519VerifyBatch pks (flipBitAt 0 (head sigs) : tail sigs) msgs `shouldBe` False

  it "rejects zero signatures and wrong lengths" $ do
    (pk, _) <- ed25519Keygen
    ed25519VerifyResult pk (B.replicate 64 0) "test message" `shouldBe` Ed25519VerificationFailed
    ed25519VerifyResult pk (B.replicate 64 0) "" `shouldBe` Ed25519VerificationFailed
    ed25519VerifyResult (B.take 31 pk) (B.replicate 64 0) "" `shouldBe` Ed25519InvalidInputLength
    ed25519VerifyResult pk (B.replicate 63 0) "" `shouldBe` Ed25519InvalidInputLength

  it "rejects a public key that is not a curve point" $ do
    -- y = 2 is not the y coordinate of any edwards25519 point.
    let pk = B.cons 2 (B.replicate 31 0)
    refDecode pk `shouldBe` Nothing
    ed25519VerifyResult pk (B.replicate 64 0) "" `shouldBe` Ed25519InvalidPublicKey

  it "requires a canonical S (S < l)" $ do
    let (seed, pk, msg, sig) = head rfc
        (r, s) = B.splitAt 32 (unhex sig)
        sInt = B.foldr' (\w acc -> acc * 256 + fromIntegral w) 0 s :: Integer
        enc = encLE
    seed `shouldSatisfy` (not . null)
    ed25519Verify (unhex pk) (r <> enc sInt) (unhex msg) `shouldBe` True
    ed25519Verify (unhex pk) (r <> enc (sInt + groupOrderL)) (unhex msg) `shouldBe` False
    ed25519Verify (unhex pk) (r <> enc (sInt + 2 * groupOrderL)) (unhex msg) `shouldBe` False

  it "pins the IACR 2020/1244 cases as hogd does (ZIP 215)" $
    forM_ (zip [0 :: Int ..] EC.iacr20201244Vectors) $ \(i, (msg, pk, sig, shouldFail)) ->
      (i, ed25519Verify (unhex pk) (unhex sig) (unhex msg)) `shouldBe` (i, not shouldFail)

  it "accepts every Zcash ZIP 215 small-order vector for any message" $ do
    length EC.zcashVectors `shouldBe` 196
    forM_ EC.zcashVectors $ \(pk, sig) -> do
      (pk, sig, ed25519Verify (unhex pk) (unhex sig) "Zcash") `shouldBe` (pk, sig, True)
      (pk, sig, ed25519Verify (unhex pk) (unhex sig) "Abc") `shouldBe` (pk, sig, True)

  it "decodes points exactly as curve25519-dalek (non-canonical y, negative zero)" $ do
    let p = 2 ^ (255 :: Int) - 19 :: Integer
        enc = encLE
        ys = [0 .. 40] ++ [p - 20 .. p - 1] ++ [p .. 2 ^ (255 :: Int) - 1]
        encodings = [enc (y .|. s) | y <- ys, s <- [0, 1 `shiftL` 255]]
    forM_ encodings $ \e -> do
      let ours = case pointDecode e of
            CryptoPassed pt -> Just (pointEncode pt :: ByteString)
            CryptoFailed _ -> Nothing
      (tohex e, ours) `shouldBe` (tohex e, refDecode e)
    -- Some of these really are non-canonical points: y = p + 1 is y = 1.
    refDecode (enc (p + 1)) `shouldBe` Just (enc 1)
    refDecode (enc (1 .|. (1 `shiftL` 255))) `shouldBe` Just (enc 1)

  it "decodes random encodings exactly as curve25519-dalek" $
    property $ forAll (vectorOf 32 arbitrary) $ \ws ->
      let e = B.pack ws
          ours = case pointDecode e of
            CryptoPassed pt -> Just (pointEncode pt :: ByteString)
            CryptoFailed _ -> Nothing
       in ours === refDecode e

  it "matches Wycheproof's Ed25519 vectors but for ZIP 215's negative-zero R" $
    withWycheproof "ed25519_test.json" $ \doc -> do
      results <- fmap concat $ forM (J.arr (J.field "testGroups" doc)) $ \group -> do
        let pk = unhex (J.str (J.field "pk" (J.field "publicKey" group)))
        forM (J.arr (J.field "tests" group)) $ \t -> do
          let tcId = read (case J.field "tcId" t of J.Number n -> n; v -> error (show v)) :: Int
              sig = unhex (J.str (J.field "sig" t))
              msg = unhex (J.str (J.field "msg" t))
              expected = J.str (J.field "result" t)
              got = ed25519Verify pk sig msg
          expected `shouldSatisfy` (`elem` ["valid", "invalid"])
          pure (tcId, expected == "valid", got, J.str (J.field "comment" t))
      length results `shouldBe` 151
      let differ = [(i, c) | (i, want, got, c) <- results, want /= got]
      -- Every valid signature verifies.
      [(i, c) | (i, True, False, c) <- results] `shouldBe` []
      -- The one vector Wycheproof rejects and ZIP 215 accepts: R is
      -- "negative zero" (y = 1 with the sign bit of x set), which
      -- curve25519-dalek decodes to the identity, as it does for A in the
      -- Zcash vectors.
      map fst differ `shouldBe` [151]
      [(i, want) | (i, want, _, _) <- results, i == 151] `shouldBe` [(151, False)]

------------------------------------------------------------------------------
-- ML-DSA-65

mldsaSpec :: Spec
mldsaSpec = describe "ML-DSA-65" $ do
  let kMsg = "test message" :: ByteString
      zeroRnd = B.replicate 32 0

  it "signs and verifies (SignAndVerify)" $ do
    key <- mldsa65Generate
    Just sig <- mldsa65Sign key kMsg ""
    B.length sig `shouldBe` kMlDsa65SignatureBytes
    B.length (mldsa65PublicKey key) `shouldBe` kMlDsa65PublicKeyBytes
    mldsa65Verify (mldsa65PublicKey key) sig kMsg `shouldBe` True
    mldsa65Verify (mldsa65PublicKey key) sig "wrong message" `shouldBe` False
    mldsa65Verify (mldsa65PublicKey key) sig "" `shouldBe` False
    other <- mldsa65Generate
    mldsa65Verify (mldsa65PublicKey other) sig kMsg `shouldBe` False

  it "signs the empty message (EmptyMessage)" $ do
    key <- mldsa65Generate
    Just sig <- mldsa65Sign key "" ""
    mldsa65Verify (mldsa65PublicKey key) sig "" `shouldBe` True
    mldsa65Verify (mldsa65PublicKey key) sig "\0" `shouldBe` False

  it "signs the context string (ContextIsSigned)" $ do
    key <- mldsa65Generate
    let pk = mldsa65PublicKey key
    Just sig <- mldsa65Sign key kMsg "context"
    mldsa65VerifyCtx pk sig kMsg "context" `shouldBe` True
    mldsa65Verify pk sig kMsg `shouldBe` False
    mldsa65VerifyCtx pk sig kMsg "other context" `shouldBe` False
    mldsa65Verify pk sig ("context" <> kMsg) `shouldBe` False
    Just noCtx <- mldsa65Sign key kMsg ""
    mldsa65VerifyCtx pk noCtx kMsg "context" `shouldBe` False

  it "caps the context at 255 bytes (ContextLengthLimit)" $ do
    key <- mldsa65Generate
    let longest = BC.replicate kMlDsaMaxContextBytes 'c'
        tooLong = BC.replicate (kMlDsaMaxContextBytes + 1) 'c'
    Just sig <- mldsa65Sign key kMsg longest
    mldsa65VerifyCtx (mldsa65PublicKey key) sig kMsg longest `shouldBe` True
    s1 <- mldsa65Sign key kMsg tooLong
    s1 `shouldSatisfy` isNothing
    mldsa65SignWithRnd key kMsg tooLong zeroRnd `shouldSatisfy` isNothing
    mldsa65VerifyCtx (mldsa65PublicKey key) sig kMsg tooLong `shouldBe` False

  it "hedges signatures (HedgedSignaturesDiffer)" $ do
    key <- mldsa65Generate
    Just s1 <- mldsa65Sign key kMsg ""
    Just s2 <- mldsa65Sign key kMsg ""
    s1 `shouldNotBe` s2
    mldsa65Verify (mldsa65PublicKey key) s1 kMsg `shouldBe` True
    mldsa65Verify (mldsa65PublicKey key) s2 kMsg `shouldBe` True

  it "repeats signatures for a fixed rnd (SigningWithFixedRndRepeats)" $ do
    key <- mldsa65Generate
    let s1 = mldsa65SignWithRnd key kMsg "" zeroRnd
        s3 = mldsa65SignWithRnd key kMsg "" (B.cons 1 (B.replicate 31 0))
    s1 `shouldSatisfy` isJust
    s1 `shouldBe` mldsa65SignWithRnd key kMsg "" zeroRnd
    s3 `shouldSatisfy` isJust
    s3 `shouldNotBe` s1
    fmap (\s -> mldsa65Verify (mldsa65PublicKey key) s kMsg) s3 `shouldBe` Just True

  it "derives keys from seeds (KeysFromSeed)" $ do
    let seed = B.replicate 32 0x2a
        k1 = mldsa65FromSeed seed
        k2 = mldsa65FromSeed seed
        k3 = mldsa65FromSeed (flipBitAt 0 seed)
    mldsa65PublicKey k1 `shouldBe` mldsa65PublicKey k2
    mldsa65SignWithRnd k1 kMsg "" zeroRnd `shouldBe` mldsa65SignWithRnd k2 kMsg "" zeroRnd
    mldsa65PublicKey k1 `shouldNotBe` mldsa65PublicKey k3

  it "rejects any modified signature or key (RejectsModifiedSignature/PublicKey)" $ do
    key <- mldsa65Generate
    let pk = mldsa65PublicKey key
    Just sig <- mldsa65Sign key kMsg ""
    forM_ [0, 97 .. B.length sig - 1] $ \i ->
      (i, mldsa65Verify pk (flipBitAt i sig) kMsg) `shouldBe` (i, False)
    let lastFlipped = B.init sig `B.snoc` (B.last sig `xor` 0x80)
    mldsa65Verify pk lastFlipped kMsg `shouldBe` False
    forM_ [0, 101 .. B.length pk - 1] $ \i ->
      (i, mldsa65Verify (flipBitAt i pk) sig kMsg) `shouldBe` (i, False)
    -- An all-zero signature is malformed and rejected.
    mldsa65Verify pk (B.replicate kMlDsa65SignatureBytes 0) kMsg `shouldBe` False

  it "rejects keys and signatures of the wrong length" $ do
    key <- mldsa65Generate
    Just sig <- mldsa65Sign key kMsg ""
    let pk = mldsa65PublicKey key
    mldsa65Verify pk sig kMsg `shouldBe` True
    mldsa65Verify (B.init pk) sig kMsg `shouldBe` False
    mldsa65Verify (pk <> "\0") sig kMsg `shouldBe` False
    mldsa65Verify pk (B.init sig) kMsg `shouldBe` False
    mldsa65Verify pk (sig <> "\0") kMsg `shouldBe` False

  it "matches CCTV's accumulated known answers (AccumulatedKnownAnswers)" $ do
    let seeds = BA.convert (H.hash B.empty :: Digest (SHAKE128 25600)) :: ByteString
        step :: Context (SHAKE128 256) -> Int -> Context (SHAKE128 256)
        step acc i =
          let key = mldsa65FromSeed (B.take 32 (B.drop (32 * i) seeds))
              pk = mldsa65PublicKey key
              sig = fromMaybe (error "sign failed") (mldsa65SignWithRnd key "" "" zeroRnd)
           in if mldsa65Verify pk sig ""
                then hashUpdates acc [pk, sig]
                else error ("iteration " ++ show i ++ " does not verify")
        digest = hashFinalize (foldl step hashInit [0 .. 99])
    tohex (BA.convert digest) `shouldBe` "8358a1843220194417cadbc2651295cd8fc65125b5a5c1a239a16dc8b57ca199"

  it "verifies Wycheproof's ML-DSA-65 vectors exactly as labeled" $
    withWycheproof "mldsa_65_verify_test.json" $ \doc -> do
      counts <- forM (J.arr (J.field "testGroups" doc)) $ \group -> do
        let pk = unhex (J.str (J.field "publicKey" group))
            tests = J.arr (J.field "tests" group)
        forM_ tests $ \t -> do
          let tcId = J.field "tcId" t
              sig = unhex (J.str (J.field "sig" t))
              msg = unhex (J.str (J.field "msg" t))
              ctx = maybe B.empty (unhex . J.str) (J.fieldMaybe "ctx" t)
              result = J.str (J.field "result" t)
          result `shouldSatisfy` (`elem` ["valid", "invalid"])
          if B.length pk /= kMlDsa65PublicKeyBytes || B.length sig /= kMlDsa65SignatureBytes
            then do
              -- A key or signature of the wrong length has no
              -- representation in C++, so it never reaches mldsa65_verify.
              (tcId, hasFlag "IncorrectPublicKeyLength" t || hasFlag "IncorrectSignatureLength" t)
                `shouldBe` (tcId, True)
              (tcId, result) `shouldBe` (tcId, "invalid")
            else pure ()
          (tcId, mldsa65VerifyCtx pk sig msg ctx) `shouldBe` (tcId, result == "valid")
        pure (length tests)
      J.Number (show (sum counts)) `shouldBe` J.field "numberOfTests" doc

  it "signs Wycheproof's ML-DSA-65 seed vectors byte for byte" $
    withWycheproof "mldsa_65_sign_seed_test.json" $ \doc -> do
      forM_ (J.arr (J.field "testGroups" doc)) $ \group -> do
        let seed = unhex (J.str (J.field "privateSeed" group))
            pk = unhex (J.str (J.field "publicKey" group))
            tests = J.arr (J.field "tests" group)
        if B.length seed /= kMlDsaSeedBytes
          then forM_ tests $ \t -> do
            -- A seed of the wrong length has no representation in C++.
            hasFlag "IncorrectPrivateKeyLength" t `shouldBe` True
            J.str (J.field "result" t) `shouldBe` "invalid"
          else do
            let key = mldsa65FromSeed seed
            mldsa65PublicKey key `shouldBe` pk
            forM_ tests $ \t -> do
              let tcId = J.field "tcId" t
                  result = J.str (J.field "result" t)
                  ctx = maybe B.empty (unhex . J.str) (J.fieldMaybe "ctx" t)
                  rnd = maybe zeroRnd (unhex . J.str) (J.fieldMaybe "rnd" t)
              -- "Internal" vectors sign an external mu, which hogd has no
              -- interface for.
              unless (hasFlag "Internal" t) $ do
                let msg = unhex (J.str (J.field "msg" t))
                    sig = mldsa65SignWithRnd key msg ctx rnd
                if result == "valid"
                  then do
                    (tcId, sig) `shouldBe` (tcId, Just (unhex (J.str (J.field "sig" t))))
                    (tcId, mldsa65VerifyCtx pk (fromMaybe B.empty sig) msg ctx) `shouldBe` (tcId, True)
                  else do
                    (tcId, hasFlag "InvalidContext" t) `shouldBe` (tcId, True)
                    (tcId, sig) `shouldBe` (tcId, Nothing)
