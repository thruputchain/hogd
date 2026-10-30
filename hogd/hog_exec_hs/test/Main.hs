module Main (main) where

import Test.Hspec
import qualified ContractTraitSpec
import qualified CryptoSpec
import qualified DefaultWalletSpec
import qualified NativeTokenSpec
import qualified TablesSpec
import qualified XdrSpec

main :: IO ()
main = hspec $ do
  describe "Xdr" XdrSpec.spec
  describe "Crypto" CryptoSpec.spec
  describe "Tables" TablesSpec.spec
  describe "ContractTrait" ContractTraitSpec.spec
  describe "NativeToken" NativeTokenSpec.spec
  describe "DefaultWallet" DefaultWalletSpec.spec
