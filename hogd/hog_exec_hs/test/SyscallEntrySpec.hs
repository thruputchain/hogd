-- | The syscall boundary: argument parsing, aliasing
-- (components/alias_guard_test.cc) and the shared wrapper's rules.
module SyscallEntrySpec (spec) where

import qualified Data.ByteString as B
import Data.IORef
import Hogd.Exec.Common.Status
import Hogd.Exec.Runtime.GasCostTable (kGenericBadSyscallCost, kSyscallBaseCost)
import Hogd.Exec.Runtime.SyscallEntry
import Hogd.Xdr.Syscall (SyscallNo (..))
import Test.Hspec

memory :: Int -> IO (GuestMemory, IORef B.ByteString)
memory n = do
  ref <- newIORef (B.pack (map fromIntegral [0 .. n - 1]))
  let rd off len = B.take len . B.drop off <$> readIORef ref
      wr off bs = modifyIORef' ref (\m -> B.take off m <> bs <> B.drop (off + B.length bs) m)
  pure (GuestMemory n rd wr, ref)

-- | Register (offset, length) spans in order; report whether they alias.
aliasOk :: [(Int, Int)] -> IO Bool
aliasOk spans = do
  (mem, _) <- memory 4096
  p <- runArgParser mem (concat [[fromIntegral o, fromIntegral l] | (o, l) <- spans]) (mapM (const pSpanIn) spans)
  pure (paAliasOk p)

roMeta, noReturnMeta :: SyscallMeta
roMeta = SyscallMeta (SyscallNo 0) True True
noReturnMeta = SyscallMeta (SyscallNo 6) True False

-- The read-only metadata never consults the handler.
noHandler :: a
noHandler = error "handler not used"

spec :: Spec
spec = do
  describe "alias guard" $ do
    it "accepts one range" $ aliasOk [(10, 20)] `shouldReturn` True
    it "accepts adjacent ranges" $ aliasOk [(100, 16), (116, 16)] `shouldReturn` True
    it "rejects identical ranges" $ aliasOk [(100, 16), (100, 16)] `shouldReturn` False
    it "rejects partial overlap" $ aliasOk [(100, 16), (108, 16)] `shouldReturn` False
    it "rejects a contained range" $ aliasOk [(100, 32), (108, 8)] `shouldReturn` False
    it "rejects a zero-length range inside a prior one" $ aliasOk [(100, 16), (108, 0)] `shouldReturn` False
    it "accepts zero-length ranges at a prior range's edges" $ aliasOk [(100, 16), (100, 0), (116, 0)] `shouldReturn` True
    it "rejects a range over a zero-length prior" $ aliasOk [(108, 0), (100, 16)] `shouldReturn` False
    it "accepts a range touching a zero-length prior" $ aliasOk [(100, 0), (100, 16)] `shouldReturn` True
    it "accepts two zero-length ranges at one point" $ aliasOk [(100, 0), (100, 0)] `shouldReturn` True

  describe "argument parsing" $ do
    it "faults on an out-of-bounds span" $ do
      (mem, _) <- memory 64
      p <- runArgParser mem [60, 8] pSpanIn
      paValid p `shouldBe` False
    it "accepts a span ending exactly at the end of memory" $ do
      (mem, _) <- memory 64
      p <- runArgParser mem [56, 8] pSpanIn
      (paValid p, paValue p) `shouldBe` (True, B.pack [56 .. 63])
    it "treats 0xffffffff as an absent optional input" $ do
      (mem, _) <- memory 64
      p <- runArgParser mem [maxBound] (pOptRefIn 32)
      (paValid p, paValue p) `shouldBe` (True, Nothing)
    it "rejects a bool slot above 1" $ do
      (mem, _) <- memory 64
      p <- runArgParser mem [2] pBool
      paValid p `shouldBe` False

  describe "wrapAfterParse" $ do
    let run m valid alias gas body = wrapAfterParse m valid alias noHandler gas () (\g -> body g)
    it "charges the bad-syscall cost plus the crossing for a fault" $ do
      ((st, g), _) <- run roMeta False True 100000 (\_ -> pure ((Right KSyscallErrorCodeNone, 0), ()))
      (st, g) `shouldBe` (Right KSyscallErrorCodeFault, kGenericBadSyscallCost + kSyscallBaseCost)
    it "runs out of gas when the crossing cannot be paid" $ do
      ((st, g), _) <- run roMeta True True kSyscallBaseCost (\_ -> pure ((Right KSyscallErrorCodeNone, 0), ()))
      (st, g) `shouldBe` (Left HostFnErrorCode_OutOfGas, kSyscallBaseCost)
    it "hands the body the gas left after the crossing" $ do
      seen <- newIORef 0
      ((st, g), out) <- run roMeta True True 1000 (\g -> writeIORef seen g >> pure ((Right KSyscallErrorCodeNone, 7), ()))
      readIORef seen `shouldReturn` (1000 - kSyscallBaseCost)
      (st, g, out) `shouldBe` (Right KSyscallErrorCodeNone, 7 + kSyscallBaseCost, Just ())
    it "turns exact exhaustion into OutOfGas" $ do
      ((st, g), _) <- run roMeta True True 1000 (\g -> pure ((Right KSyscallErrorCodeNone, g), ()))
      (st, g) `shouldBe` (Left HostFnErrorCode_OutOfGas, 1000)
    it "disarms outputs on a syscall error code" $ do
      (_, out) <- run roMeta True True 1000 (\_ -> pure ((Right KSyscallErrorCodeInvalidSyscallInput, 0), ()))
      out `shouldBe` Nothing
    it "turns a returned code from a no-return syscall into Exit" $ do
      ((st, _), _) <- wrapAfterParse noReturnMeta True True noHandler 1000 () (\_ -> pure ((Right KSyscallErrorCodeInvalidSyscallInput, 0), ()))
      st `shouldBe` Left Exit
