module Main (main) where

import Test.Hspec
import qualified XdrSpec

main :: IO ()
main = hspec $ do
  describe "Xdr" XdrSpec.spec
