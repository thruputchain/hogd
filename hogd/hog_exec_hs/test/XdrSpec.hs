module XdrSpec (spec) where

import qualified Data.ByteString as B
import Data.Int
import Data.Word
import Hogd.Xdr
import Test.Hspec
import Test.QuickCheck
import X23.Xdr

spec :: Spec
spec = do
  describe "cxs integers" $ do
    it "encodes small unsigned values in one byte" $
      cxsEncode (62 :: Word64) `shouldBe` B.pack [62]
    it "encodes 63 with a one-byte payload" $
      cxsEncode (63 :: Word64) `shouldBe` B.pack [63, 63]
    it "encodes -1 in one byte" $
      cxsEncode (-1 :: Int64) `shouldBe` B.pack [62]
    it "round-trips Int64" $ property $ \x -> cxsDecode (cxsEncode (x :: Int64)) == Right x
    it "round-trips Word32" $ property $ \x -> cxsDecode (cxsEncode (x :: Word32)) == Right x
    it "round-trips Int32" $ property $ \x -> cxsDecode (cxsEncode (x :: Int32)) == Right x
    it "rejects a non-canonical encoding" $
      (cxsDecode (B.pack [63, 5]) :: Either String Word64) `shouldSatisfy` isLeft
  describe "xdr" $ do
    it "pads opaque data with zeros" $
      xdrEncodeStrict (uint128 1 2) `shouldBe` B.pack ([0, 0, 0, 0, 0, 0, 0, 1] ++ [0, 0, 0, 0, 0, 0, 0, 2])
    it "decodes an open enum value" $
      (xdrDecode (B.pack [0, 0, 0, 99]) :: Either String ExecResult) `shouldBe` Right (ExecResult 99)
    it "rejects nonzero padding" $
      runGet 16 (getVarOpaque 10) (B.pack [0, 0, 0, 1, 7, 0, 0, 1]) `shouldSatisfy` isLeft
  where
    isLeft = either (const True) (const False)


uint128 :: Word64 -> Word64 -> X_uint128_xdr
uint128 h l = X_uint128_xdr {highbits = h, lowbits = l}

