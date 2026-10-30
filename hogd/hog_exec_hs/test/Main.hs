module Main (main) where

import Test.Hspec
import qualified Rv32Spec
import qualified XdrSpec

main :: IO ()
main = hspec $ do
  describe "Xdr" XdrSpec.spec
  describe "Rv32" Rv32Spec.spec
