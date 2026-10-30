module Main (main) where

import Test.Hspec
import qualified TablesSpec
import qualified XdrSpec

main :: IO ()
main = hspec $ do
  describe "Xdr" XdrSpec.spec
  describe "Tables" TablesSpec.spec
