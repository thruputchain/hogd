-- | The syscall boundary (runtime/syscall_entry*.h, syscall_arg_params.h,
-- syscall_argument_parsers.h, components/alias_guard.h).
--
-- A syscall body is a typed function of already-parsed inputs that returns
-- its result, the gas it wants charged, and its staged by-reference outputs.
-- 'wrapAfterParse' applies the checks every syscall shares (argument faults,
-- aliasing, read-only frames, the crossing cost, gas exhaustion, syscalls
-- that cannot return).  Two front ends feed it: 'ArgParser' decodes RV32
-- registers and guest memory, and native contracts pass host values directly.
module Hogd.Exec.Runtime.SyscallEntry
  ( -- * Guest memory
    GuestMemory (..)
  , OutSpan (..)
  , hostOutSpan
  , copyToGuestBuffer
    -- * Metadata and results
  , SyscallMeta (..)
  , InnerResult
  , SyscallResult
  , wrapAfterParse
    -- * Argument parsing
  , ArgParser
  , OutRef (..)
  , ParsedArgs (..)
  , runArgParser
  , pBool
  , pWord32
  , pInt32
  , pSpanIn
  , pSpanOut
  , pRefIn
  , pRefOut
  , pOptRefIn
  , writeOutRefs
  ) where

import Control.Exception (SomeException, evaluate, try)
import Control.Monad (forM_)
import qualified Data.ByteString as B
import Data.IORef
import Data.Int
import Data.Word
import Hogd.Exec.Common.Status
import Hogd.Exec.Log
import Hogd.Exec.Runtime.GasCostTable (kGenericBadSyscallCost, kSyscallBaseCost)
import Hogd.Exec.Runtime.SyscallHandler (isReadOnly)
import Hogd.Exec.Runtime.Types
import Hogd.Xdr.Syscall (SyscallNo)

-- | A contract's linear memory.
data GuestMemory = GuestMemory
  { gmSize :: !Int
  , gmRead :: Int -> Int -> IO B.ByteString
  -- ^ offset, length (bounds already checked)
  , gmWrite :: Int -> B.ByteString -> IO ()
  }

-- | A caller-provided output buffer.
data OutSpan = OutSpan
  { osLen :: !Int
  , osWrite :: B.ByteString -> IO ()
  -- ^ writes a prefix of at most 'osLen' bytes
  }

-- | An output buffer in host memory (native contracts).
hostOutSpan :: Int -> IO (OutSpan, IO B.ByteString)
hostOutSpan n = do
  ref <- newIORef (B.replicate n 0)
  let write bs = modifyIORef' ref (\old -> bs <> B.drop (B.length bs) old)
  pure (OutSpan n write, readIORef ref)

-- | Copy as much of @src@ as fits; return src's full length (a value larger
-- than the buffer means the output was truncated).
copyToGuestBuffer :: OutSpan -> B.ByteString -> IO Word32
copyToGuestBuffer dst src = do
  let n = min (osLen dst) (B.length src)
  if n > 0 then osWrite dst (B.take n src) else pure ()
  pure (fromIntegral (B.length src))

data SyscallMeta = SyscallMeta
  { smNo :: !SyscallNo
  , smReadOnly :: !Bool
  -- ^ allowed in a read-only frame
  , smCanReturn :: !Bool
  -- ^ whether the syscall can return to the guest at all
  }

-- | What a body reports: the status the sandbox acts on and the gas it wants
-- charged (before the crossing cost).
type InnerResult = (HostFnStatus SyscallErrorCode, Word64)

-- | What the sandbox gets: status and total gas charged.
type SyscallResult = (HostFnStatus SyscallErrorCode, Word64)

saturatingAdd :: Word64 -> Word64 -> Word64
saturatingAdd a b = let s = a + b in if s < a then maxBound else s

-- | wrap_after_parse.  The body receives the gas left after the crossing.
-- Returns the result, the gas to charge, and the staged outputs if they are
-- still armed (they are disarmed by every rejection and by any non-None
-- syscall error code).
wrapAfterParse
  :: SyscallMeta
  -> Bool
  -- ^ every argument parsed in bounds
  -> Bool
  -- ^ no two argument regions alias
  -> SystemCallHandler
  -> Word64
  -- ^ gas remaining in the frame
  -> o
  -- ^ the outputs' initial (zero) values
  -> (Word64 -> IO (InnerResult, o))
  -> IO (SyscallResult, Maybe o)
wrapAfterParse meta allValid aliasOk h gasRemaining zeroOut body = do
  ro <- if smReadOnly meta then pure False else isReadOnly h
  (bodyRes, out, armed0) <-
    if not allValid || not aliasOk
      then do
        if not aliasOk && allValid then logTrace ("Syscall " ++ show (smNo meta) ++ " faulted due to aliased memory regions") else pure ()
        pure ((Right KSyscallErrorCodeFault, kGenericBadSyscallCost), zeroOut, False)
      else
        if ro
          then do
            logTrace ("Syscall " ++ show (smNo meta) ++ " is not read-only, but called in read-only mode")
            pure ((Right KSyscallErrorCodeInvalidSyscallInput, 0), zeroOut, False)
          else
            if gasRemaining <= kSyscallBaseCost
              then pure ((Left HostFnErrorCode_OutOfGas, 0), zeroOut, True)
              else do
                r <- try (body (gasRemaining - kSyscallBaseCost) >>= \x@((st, g), _) -> evaluate st >> evaluate g >> pure x)
                case r of
                  Right (res, o) -> pure (res, o, True)
                  Left (e :: SomeException) -> do
                    logError ("unrecoverable: syscall " ++ show (smNo meta) ++ " body threw: " ++ show e)
                    pure ((Left HostFnErrorCode_Unrecoverable, 0), zeroOut, False)
  let (status, gas0) = bodyRes
      gasConsumed = saturatingAdd gas0 kSyscallBaseCost
  handleable <- case status of
    Right _ -> pure True
    Left HostFnErrorCode_Abandon -> pure False
    Left HostFnErrorCode_ValidationFailure -> pure False
    Left HostFnErrorCode_Unrecoverable -> do
      logError "Unrecoverable: syscall_entry_outer got Unrecoverable"
      pure False
    Left HostFnErrorCode_OutOfGas -> pure True
    Left Exit -> pure True
    Left HostFnErrorCode_Return -> pure True
    Left other -> panic ("Unhandled HostFnErrorCode " ++ show other)
  let (status', armed)
        | handleable && gasConsumed >= gasRemaining = (Left HostFnErrorCode_OutOfGas, armed0)
        | not (smCanReturn meta), Right _ <- status = (Left Exit, armed0)
        | Right code <- status, code /= KSyscallErrorCodeNone = (status, False)
        | otherwise = (status, armed0)
  pure ((status', min gasConsumed gasRemaining), if armed then Just out else Nothing)

------------------------------------------------------------------------------
-- Argument parsing from RV32 registers

-- | A by-reference output's location in guest memory.
data OutRef = OutRef {orAddr :: !Int, orSize :: !Int}

data PState = PState
  { psArgs :: [Word32]
  , psRanges :: [(Word64, Word64)]
  , psValid :: !Bool
  , psAliasOk :: !Bool
  , psOutRefs :: [OutRef]
  }

-- | Decodes syscall arguments from argument slots and guest memory.  Every
-- argument is parsed even after a failure, as the C++ parses them all before
-- validating, so alias registration matches.
newtype ArgParser a = ArgParser (GuestMemory -> PState -> IO (a, PState))

instance Functor ArgParser where
  fmap f (ArgParser g) = ArgParser $ \m s -> do
    (a, s') <- g m s
    pure (f a, s')

instance Applicative ArgParser where
  pure a = ArgParser $ \_ s -> pure (a, s)
  ArgParser f <*> ArgParser g = ArgParser $ \m s -> do
    (h, s1) <- f m s
    (a, s2) <- g m s1
    pure (h a, s2)

instance Monad ArgParser where
  ArgParser g >>= k = ArgParser $ \m s -> do
    (a, s1) <- g m s
    let ArgParser g' = k a
    g' m s1

data ParsedArgs a = ParsedArgs
  { paValue :: a
  , paValid :: Bool
  , paAliasOk :: Bool
  , paOutRefs :: [OutRef]
  -- ^ in argument order
  }

runArgParser :: GuestMemory -> [Word32] -> ArgParser a -> IO (ParsedArgs a)
runArgParser mem args (ArgParser p) = do
  (a, s) <- p mem (PState args [] True True [])
  pure (ParsedArgs a (psValid s) (psAliasOk s) (reverse (psOutRefs s)))

nextSlot :: ArgParser Word32
nextSlot = ArgParser $ \_ s -> case psArgs s of
  (x : rest) -> pure (x, s {psArgs = rest})
  [] -> pure (0, s)

invalid :: ArgParser ()
invalid = ArgParser $ \_ s -> pure ((), s {psValid = False})

-- | AliasGuard::check_and_register: a range overlapping any registered one is
-- a violation and is not registered.
register :: Word64 -> Word64 -> ArgParser ()
register start len = ArgParser $ \_ s ->
  let end = start + len
      overlaps = any (\(rs, re) -> start < re && rs < end) (psRanges s)
   in if overlaps
        then pure ((), s {psAliasOk = False})
        else pure ((), s {psRanges = psRanges s ++ [(start, end)]})

inBounds :: Word64 -> Word64 -> ArgParser Bool
inBounds base len = ArgParser $ \m s -> pure (base + len <= fromIntegral (gmSize m), s)

readMem :: Word64 -> Word64 -> ArgParser B.ByteString
readMem base len = ArgParser $ \m s -> do
  b <- gmRead m (fromIntegral base) (fromIntegral len)
  pure (b, s)

pBool :: ArgParser Bool
pBool = do
  v <- nextSlot
  if v > 1 then invalid >> pure False else pure (v == 1)

-- | Any 32-bit pattern is a valid uint32 / int32 / int32-backed enum.
pWord32 :: ArgParser Word32
pWord32 = nextSlot

pInt32 :: ArgParser Int32
pInt32 = fromIntegral <$> nextSlot

-- | An input byte span (offset, length).
pSpanIn :: ArgParser B.ByteString
pSpanIn = do
  off <- fromIntegral <$> nextSlot
  len <- fromIntegral <$> nextSlot
  register off len
  ok <- inBounds off len
  if ok then readMem off len else invalid >> pure B.empty

-- | An output byte span (offset, length).
pSpanOut :: ArgParser OutSpan
pSpanOut = do
  off <- fromIntegral <$> nextSlot
  len <- fromIntegral <$> nextSlot
  register off len
  ok <- inBounds off len
  if ok
    then ArgParser $ \m s -> pure (OutSpan (fromIntegral len) (gmWrite m (fromIntegral off)), s)
    else invalid >> pure (OutSpan 0 (const (pure ())))

-- | A fixed-size by-reference input.
pRefIn :: Int -> ArgParser B.ByteString
pRefIn size = do
  off <- fromIntegral <$> nextSlot
  register off (fromIntegral size)
  ok <- inBounds off (fromIntegral size)
  if ok then readMem off (fromIntegral size) else invalid >> pure (B.replicate size 0)

-- | A fixed-size by-reference output.
pRefOut :: Int -> ArgParser OutRef
pRefOut size = do
  off <- fromIntegral <$> nextSlot
  register off (fromIntegral size)
  ok <- inBounds off (fromIntegral size)
  let r = OutRef (fromIntegral off) size
  if ok
    then ArgParser $ \_ s -> pure (r, s {psOutRefs = r : psOutRefs s})
    else invalid >> pure r

-- | An optional by-reference input; offset 0xffffffff means absent.
pOptRefIn :: Int -> ArgParser (Maybe B.ByteString)
pOptRefIn size = do
  off <- nextSlot
  if off == maxBound
    then pure Nothing
    else do
      let o = fromIntegral off
      register o (fromIntegral size)
      ok <- inBounds o (fromIntegral size)
      if ok then Just <$> readMem o (fromIntegral size) else invalid >> pure Nothing

-- | Flush staged outputs into guest memory.
writeOutRefs :: GuestMemory -> [(OutRef, B.ByteString)] -> IO ()
writeOutRefs mem outs = forM_ outs $ \(OutRef addr size, bytes) ->
  gmWrite mem addr (B.take size (bytes <> B.replicate size 0))
