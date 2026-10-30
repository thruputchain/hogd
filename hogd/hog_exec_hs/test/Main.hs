module Main (main) where

import Test.Hspec
import qualified CryptoSpec
import qualified TablesSpec
import qualified ExecutionModeSpec
import qualified SyscallEntrySpec
import qualified XdrSpec

main :: IO ()
main = hspec $ do
  describe "Xdr" XdrSpec.spec
  describe "ExecutionMode" ExecutionModeSpec.spec
  describe "SyscallEntry" SyscallEntrySpec.spec
  describe "Crypto" CryptoSpec.spec
  describe "Tables" TablesSpec.spec
