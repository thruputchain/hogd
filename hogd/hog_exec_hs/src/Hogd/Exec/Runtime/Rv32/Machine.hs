{-# LANGUAGE MagicHash #-}
{-# LANGUAGE MultiWayIf #-}
{-# LANGUAGE UnboxedTuples #-}

-- | The RV32IM virtual machine: port of @machine.h@ / @machine.cc@ (the
-- canonical per-instruction interpreter) with the production-facing shape of
-- @threaded_machine.h@ (@ThreadedMachinePreInstantiation@ /
-- @ThreadedMachine@), which is what @rv32_contract.cc@ drives.
--
-- = Execution model
--
-- Code is immutable and separate from guest data memory: loads and stores
-- cannot read or modify instructions. Unaligned data accesses are allowed
-- when the entire access is in bounds; a zero-sized memory faults every
-- access. Jump and branch targets must be four-byte aligned and inside the
-- code image; falling off the image traps 'TrapBadPc' without charging gas.
-- Execution enters at PC 0 with every register zero except the method id
-- (@a0@ = low 32 bits, @a1@ = high 32 bits).
--
-- = Gas: canonical per-instruction charging
--
-- Each instruction is charged before it executes ("a valid fetched
-- instruction pays gas before any semantic trap or state mutation"); an
-- unaffordable instruction traps 'TrapOutOfGas' with zero gas left and no
-- partial effects.
--
-- Production (@ThreadedMachine@) charges a whole straight-line run at its
-- head (runs end at branches, jumps, ecall, ebreak, unimp, or a split point
-- keeping a run at most @kMaxRunGas@ = 10000), refunds the unexecuted suffix
-- of the run on a memory fault, and runs an unaffordable run speculatively
-- with a recorded deficit that is settled as (out of gas, 0 gas) at the next
-- run head, before an ecall reaches the host, or at a trap -- unless a memory
-- fault's suffix refund covers the deficit, in which case the fault was
-- affordable per instruction and is reported as 'TrapBadMemory'. Reading
-- @threaded_machine.h@ against this interpreter, every value the executor
-- observes through @rv32_contract.cc@ is identical:
--
--  * gas remaining at each ecall: the ecall terminates its run, so the run
--    charge equals the per-instruction charges through the ecall; a deficit
--    run never reaches the host (@op_ecall@ settles it), exactly when the
--    per-instruction machine runs out of gas before the ecall completes;
--  * trap kind and remaining gas at a trap: barrier traps (bad jump/branch
--    target, ebreak, unimp) end their run, so the charge is the same; a
--    memory fault refunds the suffix (@trap_bad_memory@), leaving exactly
--    the prefix through the faulting instruction charged; fall-off charges
--    nothing (the sentinel slot's @gas_to_run@ is 0);
--  * out of gas always leaves zero gas, in both engines;
--  * register and memory effects of non-trapping execution are identical
--    (the fused handlers only fuse when operand aliasing is preserved:
--    @lui;addi@ and @auipc;jalr@ require matching registers, load-use fusion
--    requires a non-x0 load destination, and the tail of a fused pair keeps
--    its standalone handler for branches into it);
--  * host gas (@HostResult::continue_with(value, gas)@) is debited before
--    @a0@ is written; an over-budget debit traps out of gas with zero gas
--    and leaves @a0@ unchanged (@apply_ecall@ == @complete_ecall@);
--  * @consume_gas@ (used by the ecall loop on host errors) zeroes the gas on
--    an over-budget amount and returns false without setting a trap: that is
--    @ThreadedMachine::consume_gas@, which differs from the canonical
--    @Machine::consume_gas@ (it also records an out-of-gas trap) -- the
--    production behaviour is the one implemented by 'consumeGas'.
--
-- What production does differently is confined to state a failed
-- invocation discards (DESIGN.md, "Gas and traps"): after an out-of-gas trap
-- the speculative run may have mutated registers and memory past the last
-- affordable instruction, and the reported trap PC differs. This
-- interpreter stops at the first unaffordable instruction instead; the
-- executor discards the frame either way.
module Hogd.Exec.Runtime.Rv32.Machine
  ( -- * Prepared (pre-instantiated) contract
    PreparedContract
  , preInstantiate
  , preparedInstructionCount
  , preparedCodeBytes
  , preparedMemoryBytes
  , preparedInitialMemory
    -- * Machines
  , Machine
  , newMachine
  , instantiate
  , reset
  , seedMethodId
  , start
  , step
  , resumeFromEcall
  , pendingEcall
  , gasRemaining
  , consumeGas
  , trap
  , halted
  , haltCode
  , pc
  , reg
    -- * Guest memory
  , memorySize
  , readGuest
  , writeGuest
  , guestMemory
  , memorySnapshot
    -- * Test hooks (@MachineTestPeer@)
  , setReg
  , setPc
  ) where

import Control.Monad (when)
import Data.Bits (complement, shiftL, testBit, unsafeShiftL, unsafeShiftR, xor, (.&.), (.|.))
import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import qualified Data.ByteString.Internal as BI
import qualified Data.ByteString.Unsafe as BU
import Data.Int (Int16, Int8)
import Data.Primitive.ByteArray
import Data.Primitive.PrimArray
import Data.Word (Word16, Word32, Word64, Word8)
import Foreign.Ptr (Ptr, castPtr)
import System.IO.Unsafe (unsafeDupablePerformIO)
import GHC.ByteOrder (ByteOrder (..), targetByteOrder)
import GHC.Exts (Int (I#), tagToEnum#)
import qualified GHC.Exts as Exts
import GHC.IO (IO (..))
import GHC.Word (Word16 (W16#), Word32 (W32#))
import Hogd.Exec.Runtime.Rv32.Encoding (loadU32Le)
import Hogd.Exec.Runtime.Rv32.Host
import Hogd.Exec.Runtime.Rv32.IsaOps
import Hogd.Exec.Runtime.Rv32.Program

------------------------------------------------------------------------------
-- Prepared contract

-- | The immutable, decode-once form of a contract
-- (@ThreadedMachinePreInstantiation@): the validated instruction stream,
-- pre-decoded into packed words, plus the initial memory image. Shareable
-- across any number of concurrent instantiations.
data PreparedContract = PreparedContract
  { pcCode :: !(PrimArray Word64)
  -- ^ One packed instruction per slot (see 'packInstruction').
  , pcCodeBytes :: !Word32
  -- ^ Instruction count * 4: the first PC past the image.
  , pcImage :: !ByteArray
  -- ^ The stored initial-memory bytes.
  , pcImageStart :: !Int
  -- ^ Data-space VA at which 'pcImage' is placed.
  , pcMemoryBytes :: !Int
  -- ^ Total data-memory size; @[0, start)@ and the tail are zero.
  }

instance Show PreparedContract where
  show p =
    "PreparedContract{instructions=" ++ show (sizeofPrimArray (pcCode p))
      ++ ", memoryBytes=" ++ show (pcMemoryBytes p)
      ++ ", imageStart=" ++ show (pcImageStart p)
      ++ ", imageBytes=" ++ show (sizeofByteArray (pcImage p))
      ++ "}"

preparedInstructionCount :: PreparedContract -> Int
preparedInstructionCount = sizeofPrimArray . pcCode

preparedCodeBytes :: PreparedContract -> Word32
preparedCodeBytes = pcCodeBytes

preparedMemoryBytes :: PreparedContract -> Word32
preparedMemoryBytes = fromIntegral . pcMemoryBytes

-- | The full zero-padded initial data memory (@memory_bytes@ long).
preparedInitialMemory :: PreparedContract -> ByteString
preparedInitialMemory p =
  B.replicate (pcImageStart p) 0
    <> byteArrayToByteString (pcImage p)
    <> B.replicate (pcMemoryBytes p - pcImageStart p - sizeofByteArray (pcImage p)) 0

-- | @ThreadedMachinePreInstantiation::create(code, memory, memory_start,
-- total_memory_size)@, the production preparation step. Checks, in this
-- order:
--
--  1. code length a multiple of four ('CodeSizeNotAligned'), then at most
--     @kCodeCeilingBytes@ ('CodeTooLarge') -- @ThreadedProgram::create@;
--  2. @total_memory_size <= kMemoryCeilingBytes@ and the image fits at
--     @memory_start@ ('MemoryTooLarge'), checked with subtraction so nothing
--     can wrap;
--  3. every word decodes. Lowering is a single backward pass, so when
--     several words are invalid the error of the LAST one is reported (the
--     canonical 'validateProgram' reports the first; the reject sets are
--     identical and every decode error maps to the same artifact rejection).
preInstantiate :: ByteString -> ByteString -> Word32 -> Word32 -> Either ProgramError PreparedContract
preInstantiate code memory memoryStart totalMemorySize
  | B.length code `mod` 4 /= 0 = Left CodeSizeNotAligned
  | B.length code > fromIntegral kCodeCeilingBytes = Left CodeTooLarge
  | totalMemorySize > kMemoryCeilingBytes
      || memoryStart > totalMemorySize
      || fromIntegral (B.length memory) > totalMemorySize - memoryStart =
      Left MemoryTooLarge
  | otherwise = do
      let n = B.length code `div` 4
      -- Backward pass, as in the lowering: report the last invalid word.
      decoded <- decodeBackward (n - 1) []
      pure
        PreparedContract
          { pcCode = primArrayFromListN n (map packInstruction decoded)
          , pcCodeBytes = fromIntegral (B.length code)
          , pcImage = byteStringToByteArray memory
          , pcImageStart = fromIntegral memoryStart
          , pcMemoryBytes = fromIntegral totalMemorySize
          }
  where
    decodeBackward i acc
      | i < 0 = Right acc
      | otherwise = case decodeInstruction (loadU32Le code (i * 4)) of
          Left e -> Left e
          Right d -> decodeBackward (i - 1) (d : acc)

-- Packed instruction layout (one Word64 per slot):
--
--   bits  0..7   op tag (fromEnum Op)
--   bits  8..13  rd           (x0 writes land in the trash slot 0)
--   bits 14..19  rs1 read slot (x0 remapped to the always-zero slot 32)
--   bits 20..25  rs2 read slot (likewise)
--   bits 26..31  gas cost
--   bits 32..63  immediate bits
--
-- Remapping x0 reads to a slot that is never written lets the hot path write
-- rd unconditionally, as @ThreadedMachine@ does with @regs_[kRegisterCount]@.
packInstruction :: DecodedInstruction -> Word64
packInstruction d =
  fromIntegral (fromEnum d.op)
    .|. (fromIntegral d.rd `unsafeShiftL` 8)
    .|. (readSlot d.rs1 `unsafeShiftL` 14)
    .|. (readSlot d.rs2 `unsafeShiftL` 20)
    .|. (d.gasCost `unsafeShiftL` 26)
    .|. (fromIntegral (bitsFromI32 d.immediate) `unsafeShiftL` 32)
  where
    readSlot :: Word8 -> Word64
    readSlot 0 = fromIntegral zeroSlot
    readSlot r = fromIntegral r

------------------------------------------------------------------------------
-- Machine

-- | A live machine for one invocation: registers, PC, data memory, gas and
-- trap state. Mutable; not safe for concurrent use. The 'PreparedContract'
-- it was instantiated from is shared read-only.
data Machine = Machine
  { mPrepared :: !PreparedContract
  , mRegs :: !(MutableByteArray Exts.RealWorld)
  -- ^ 'regSlots' Word32 slots: 0 = x0 write sink, 1..31 = x1..x31,
  -- 32 = always zero (x0 reads), 33..40 = the pending ecall's a0..a7.
  , mMem :: !(MutableByteArray Exts.RealWorld)
  , mState :: !(MutableByteArray Exts.RealWorld)
  -- ^ Word64 scalars, indexed by the @s*@ constants below.
  }

zeroSlot, ecallSlot, regSlots :: Int
zeroSlot = 32
ecallSlot = 33
regSlots = 41

sGas, sPc, sTrap, sFlags, sHaltCode, stateSlots :: Int
sGas = 0
sPc = 1
sTrap = 2
sFlags = 3
sHaltCode = 4
stateSlots = 5

flagHalted, flagPending :: Word64
flagHalted = 1
flagPending = 2

readState :: Machine -> Int -> IO Word64
readState m = readByteArray (mState m)
{-# INLINE readState #-}

writeState :: Machine -> Int -> Word64 -> IO ()
writeState m = writeByteArray (mState m)
{-# INLINE writeState #-}

-- | A fresh machine over a prepared contract (@ThreadedMachine(pre, gas)@):
-- the initial memory image copied in, all registers zero, PC 0.
newMachine :: PreparedContract -> Word64 -> IO Machine
newMachine prepared gas = do
  regs <- newByteArray (regSlots * 4)
  mem <- newByteArray (pcMemoryBytes prepared)
  st <- newByteArray (stateSlots * 8)
  let m = Machine {mPrepared = prepared, mRegs = regs, mMem = mem, mState = st}
  reset m gas
  pure m

-- | @Rv32Contract::instantiate(gas)@ followed by @seed_method_id@, as
-- @Rv32Runtime::invoke_internal@ does.
instantiate :: PreparedContract -> Word64 -> Word64 -> IO Machine
instantiate prepared methodId gas = do
  m <- newMachine prepared gas
  seedMethodId m methodId
  pure m

-- | Restores the initial memory image, zeroes the registers (entry ABI v3:
-- @sp@ too; the guest establishes its stack), and clears PC, trap, halt and
-- pending-ecall state.
reset :: Machine -> Word64 -> IO ()
reset m gas = do
  let p = mPrepared m
      imgLen = sizeofByteArray (pcImage p)
  setByteArray (mRegs m) 0 regSlots (0 :: Word32)
  setByteArray (mMem m) 0 (pcMemoryBytes p) (0 :: Word8)
  copyByteArray (mMem m) (pcImageStart p) (pcImage p) 0 imgLen
  setByteArray (mState m) 0 stateSlots (0 :: Word64)
  writeState m sGas gas
  writeState m sTrap (trapCode TrapNone)

-- | Seed the method id for guest dispatch: @a0@ = low 32 bits, @a1@ = high
-- 32 bits. Call after construction/reset and before 'start'.
seedMethodId :: Machine -> Word64 -> IO ()
seedMethodId m methodId = do
  writeByteArray (mRegs m) 10 (fromIntegral methodId :: Word32)
  writeByteArray (mRegs m) 11 (fromIntegral (methodId `unsafeShiftR` 32) :: Word32)

trapCode :: Trap -> Word64
trapCode = fromIntegral . fromEnum

gasRemaining :: Machine -> IO Word64
gasRemaining m = readState m sGas

-- | @ThreadedMachine::consume_gas@: debit @amount@; if it exceeds the
-- remaining gas, zero the gas and return 'False'. It does not record a trap
-- (the canonical @Machine@'s private helper does; production's public one,
-- used by the ecall loop for host-error gas, does not).
consumeGas :: Machine -> Word64 -> IO Bool
consumeGas m amount = do
  gas <- readState m sGas
  if amount > gas
    then writeState m sGas 0 >> pure False
    else writeState m sGas (gas - amount) >> pure True

trap :: Machine -> IO Trap
trap m = toEnum . fromIntegral <$> readState m sTrap

halted :: Machine -> IO Bool
halted m = (`testBit` 0) <$> readState m sFlags

haltCode :: Machine -> IO Word32
haltCode m = fromIntegral <$> readState m sHaltCode

-- | Current PC: the next instruction, the faulting instruction after a trap,
-- or the instruction after the ecall while one is pending.
pc :: Machine -> IO Word32
pc m = fromIntegral <$> readState m sPc

-- | Architectural register @x0..x31@ (x0 reads 0; out of range reads 0).
reg :: Machine -> Int -> IO Word32
reg m i
  | i <= 0 || i >= kRegisterCount = pure 0
  | otherwise = readByteArray (mRegs m) i

-- | The arguments of the pending ecall (@a0..a7@ as they were when the
-- ecall executed), if the machine is suspended at one.
pendingEcall :: Machine -> IO (Maybe EcallArgs)
pendingEcall m = do
  flags <- readState m sFlags
  if flags .&. flagPending == 0
    then pure Nothing
    else do
      let r i = readByteArray (mRegs m) (ecallSlot + i) :: IO Word32
      Just <$> (EcallArgs <$> r 0 <*> r 1 <*> r 2 <*> r 3 <*> r 4 <*> r 5 <*> r 6 <*> r 7)

-- | Test hook (@MachineTestPeer::set_reg@): writes x1..x31; x0 is ignored.
setReg :: Machine -> Int -> Word32 -> IO ()
setReg m i v
  | i <= 0 || i >= kRegisterCount = pure ()
  | otherwise = writeByteArray (mRegs m) i v

-- | Test hook (@MachineTestPeer::set_pc@). An invalid PC traps 'TrapBadPc'
-- at the next fetch without charging gas.
setPc :: Machine -> Word32 -> IO ()
setPc m v = writeState m sPc (fromIntegral v)

------------------------------------------------------------------------------
-- Guest memory

memorySize :: Machine -> Int
memorySize = pcMemoryBytes . mPrepared

-- | The live guest data memory (for syscall handlers that need direct
-- access; 'readGuest' / 'writeGuest' are the bounds-checked forms).
guestMemory :: Machine -> MutableByteArray Exts.RealWorld
guestMemory = mMem

-- | Bounds-checked copy of guest bytes @[ptr, ptr + len)@
-- (@MachineView::read_memory@). 'Nothing' if any byte is out of bounds; a
-- zero-length range ending exactly at the end of memory is in bounds.
readGuest :: Machine -> Word32 -> Word32 -> IO (Maybe ByteString)
readGuest m ptr len
  | not (memoryRangeInBounds (fromIntegral (memorySize m)) ptr (fromIntegral len)) = pure Nothing
  | len == 0 = pure (Just B.empty)
  | otherwise =
      Just
        <$> BI.create
          (fromIntegral len)
          (\dst -> copyMutableByteArrayToPtr dst (mMem m) (fromIntegral ptr) (fromIntegral len))

-- | Bounds-checked write of @bytes@ at guest address @ptr@
-- (@MachineView::write_memory@). Returns 'False' and writes nothing if the
-- range is out of bounds. Writes are not rolled back by any later trap.
writeGuest :: Machine -> Word32 -> ByteString -> IO Bool
writeGuest m ptr bytes
  | not (memoryRangeInBounds (fromIntegral (memorySize m)) ptr (fromIntegral len)) = pure False
  | len == 0 = pure True
  | otherwise = do
      BU.unsafeUseAsCString bytes $ \src ->
        copyPtrToMutableByteArray (mMem m) (fromIntegral ptr) (castPtr src :: Ptr Word8) len
      pure True
  where
    len = B.length bytes

-- | A copy of the whole guest memory.
memorySnapshot :: Machine -> IO ByteString
memorySnapshot m = do
  Just bs <- readGuest m 0 (fromIntegral (memorySize m))
  pure bs

------------------------------------------------------------------------------
-- Driving

-- | Run from the current PC to the next yield: 'StepEcall' (args via
-- 'pendingEcall'), 'StepTrap', or 'StepHalted' (only after a host halt).
-- On a halted/trapped machine this returns the terminal result again; with
-- an ecall still pending it re-yields 'StepEcall' (the host result must be
-- applied with 'resumeFromEcall').
start :: Machine -> IO StepResult
start m = do
  flags <- readState m sFlags
  if
    | flags .&. flagPending /= 0 -> pure StepEcall
    | flags .&. flagHalted /= 0 -> haltedResult m
    | otherwise -> runLoop m

-- | Execute a single instruction (canonical @Machine::step@); returns
-- 'StepContinue' after an ordinary instruction.
step :: Machine -> IO StepResult
step m = do
  flags <- readState m sFlags
  if
    | flags .&. flagPending /= 0 -> pure StepEcall
    | flags .&. flagHalted /= 0 -> haltedResult m
    | otherwise -> stepLoop m

-- | Apply the host's result to the pending ecall and, if the guest
-- continues, run to the next yield (@resume_from_ecall@). Host gas is
-- debited first; if it exceeds the remaining gas the machine traps
-- 'TrapOutOfGas' with zero gas and @a0@ is not written. Calling this with no
-- pending ecall is a programming error.
resumeFromEcall :: Machine -> HostResult -> IO StepResult
resumeFromEcall m result = do
  flags <- readState m sFlags
  when (flags .&. flagPending == 0) $
    errorWithoutStackTrace "Hogd.Exec.Runtime.Rv32.Machine.resumeFromEcall: no pending ecall"
  writeState m sFlags (flags .&. complement flagPending)
  gas <- readState m sGas
  let consumed = hostGasConsumed result
  if gas < consumed
    then terminalTrap m TrapOutOfGas 0
    else do
      writeState m sGas (gas - consumed)
      case result of
        HostContinue value _ -> do
          writeByteArray (mRegs m) 10 value
          runLoop m
        HostHalt code _ -> do
          writeState m sHaltCode (fromIntegral code)
          writeState m sFlags (flags .&. complement flagPending .|. flagHalted)
          pure StepHalted
        HostOutOfGas _ -> terminalTrap m TrapOutOfGas 0

-- | Records a trap at the current PC with the given remaining gas.
terminalTrap :: Machine -> Trap -> Word64 -> IO StepResult
terminalTrap m t gas = do
  writeState m sGas gas
  writeState m sTrap (trapCode t)
  flags <- readState m sFlags
  writeState m sFlags ((flags .&. complement flagPending) .|. flagHalted)
  pure StepTrap

runLoop :: Machine -> IO StepResult
runLoop m = drive False m
{-# NOINLINE runLoop #-}

stepLoop :: Machine -> IO StepResult
stepLoop m = drive True m
{-# NOINLINE stepLoop #-}

haltedResult :: Machine -> IO StepResult
haltedResult m = do
  t <- trap m
  pure (if t == TrapNone then StepHalted else StepTrap)

-- | The interpreter. @single@ is a compile-time constant at both call sites
-- ('runLoop', 'stepLoop'), so each gets its own specialised loop.
drive :: Bool -> Machine -> IO StepResult
{-# INLINE drive #-}
drive single m = do
  pc0 <- readState m sPc
  gas0 <- readState m sGas
  loop (fromIntegral pc0) gas0
  where
    !prep = mPrepared m
    !code = pcCode prep
    !codeBytes = pcCodeBytes prep
    !regs = mRegs m
    !mem = mMem m
    !memSize = fromIntegral (pcMemoryBytes prep) :: Word64

    readR :: Int -> IO Word32
    readR = readByteArray regs
    {-# INLINE readR #-}
    writeR :: Int -> Word32 -> IO ()
    writeR = writeByteArray regs
    {-# INLINE writeR #-}

    save :: Word32 -> Word64 -> IO ()
    save !p !g = writeState m sPc (fromIntegral p) >> writeState m sGas g
    {-# INLINE save #-}

    trapAt :: Trap -> Word32 -> Word64 -> IO StepResult
    trapAt t p g = do
      save p g
      writeState m sTrap (trapCode t)
      flags <- readState m sFlags
      writeState m sFlags (flags .|. flagHalted)
      pure StepTrap

    -- Every dynamic target is rechecked so computed control flow cannot
    -- escape the code image (@validate_control_target@).
    validTarget :: Word32 -> Bool
    validTarget t = t .&. 3 == 0 && t < codeBytes
    {-# INLINE validTarget #-}

    inBounds :: Word32 -> Word64 -> Bool
    inBounds addr width = fromIntegral addr + width <= memSize
    {-# INLINE inBounds #-}

    loop :: Word32 -> Word64 -> IO StepResult
    loop !p !gas
      -- Invalid fetch (fall-off, or a test-set PC) traps without gas.
      | p .&. 3 /= 0 || p >= codeBytes = trapAt TrapBadPc p gas
      | otherwise = do
          let !w = indexPrimArray code (fromIntegral (p `unsafeShiftR` 2))
              !cost = (w `unsafeShiftR` 26) .&. 0x3f
          -- Consensus ordering: a valid fetched instruction pays gas before
          -- any semantic trap or state mutation; out of gas saturates to 0.
          if gas < cost
            then trapAt TrapOutOfGas p 0
            else exec p (gas - cost) w

    next :: Word32 -> Word64 -> IO StepResult
    next !p !g
      | single = save p g >> pure StepContinue
      | otherwise = loop p g
    {-# INLINE next #-}

    exec :: Word32 -> Word64 -> Word64 -> IO StepResult
    exec !p !g !w = do
      let !rdI = fromIntegral ((w `unsafeShiftR` 8) .&. 0x3f) :: Int
          !rs1I = fromIntegral ((w `unsafeShiftR` 14) .&. 0x3f) :: Int
          !rs2I = fromIntegral ((w `unsafeShiftR` 20) .&. 0x3f) :: Int
          !imm = fromIntegral (w `unsafeShiftR` 32) :: Word32
          !pn = p + 4
          rr f = do
            a <- readR rs1I
            b <- readR rs2I
            writeR rdI (f a b)
            next pn g
          {-# INLINE rr #-}
          ri f = do
            a <- readR rs1I
            writeR rdI (f a imm)
            next pn g
          {-# INLINE ri #-}
          branch cmp = do
            a <- readR rs1I
            b <- readR rs2I
            if cmp a b
              then
                let !t = p + imm
                 in if validTarget t then next t g else trapAt TrapBadPc p g
              else next pn g
          {-# INLINE branch #-}
          -- Bounds is the only data-access check; it runs after the gas
          -- debit and before any register or memory mutation.
          load width rdv = do
            base <- readR rs1I
            let !addr = base + imm
            if inBounds addr width
              then do
                v <- rdv mem (fromIntegral addr)
                writeR rdI v
                next pn g
              else trapAt TrapBadMemory p g
          {-# INLINE load #-}
          store width wrv = do
            base <- readR rs1I
            v <- readR rs2I
            let !addr = base + imm
            if inBounds addr width
              then wrv mem (fromIntegral addr) v >> next pn g
              else trapAt TrapBadMemory p g
          {-# INLINE store #-}
      case opOf w of
        OpLui -> writeR rdI imm >> next pn g
        OpAuipc -> writeR rdI (p + imm) >> next pn g
        OpJal ->
          let !t = p + imm
           in if validTarget t then writeR rdI pn >> next t g else trapAt TrapBadPc p g
        OpJalr -> do
          -- JALR clears bit 0 after adding the immediate; validate the masked
          -- target before writing rd so a bad target exposes no link write.
          base <- readR rs1I
          let !t = (base + imm) .&. complement 1
          if validTarget t then writeR rdI pn >> next t g else trapAt TrapBadPc p g
        OpBeq -> branch (==)
        OpBne -> branch (/=)
        OpBlt -> branch (\a b -> i32FromBits a < i32FromBits b)
        OpBge -> branch (\a b -> i32FromBits a >= i32FromBits b)
        OpBltu -> branch (<)
        OpBgeu -> branch (>=)
        OpLb -> load 1 (\a i -> (\b -> fromIntegral (fromIntegral b :: Int8)) <$> (readByteArray a i :: IO Word8))
        OpLh -> load 2 (\a i -> (\h -> fromIntegral (fromIntegral h :: Int16)) <$> memRead16 a i)
        OpLw -> load 4 memRead32
        OpLbu -> load 1 (\a i -> fromIntegral <$> (readByteArray a i :: IO Word8))
        OpLhu -> load 2 (\a i -> fromIntegral <$> memRead16 a i)
        OpSb -> store 1 (\a i v -> writeByteArray a i (fromIntegral v :: Word8))
        OpSh -> store 2 (\a i v -> memWrite16 a i (fromIntegral v))
        OpSw -> store 4 memWrite32
        OpAddi -> ri (+)
        OpSlti -> ri (\a b -> if i32FromBits a < i32FromBits b then 1 else 0)
        OpSltiu -> ri (\a b -> if a < b then 1 else 0)
        OpXori -> ri xor
        OpOri -> ri (.|.)
        OpAndi -> ri (.&.)
        -- RV32 masks register and immediate shift amounts to five bits.
        OpSlli -> ri (\a b -> a `unsafeShiftL` fromIntegral (b .&. 0x1f))
        OpSrli -> ri (\a b -> a `unsafeShiftR` fromIntegral (b .&. 0x1f))
        OpSrai -> ri (\a b -> arithmeticShiftRight a (fromIntegral (b .&. 0x1f)))
        OpAdd -> rr (+)
        OpSub -> rr (-)
        OpSll -> rr (\a b -> a `unsafeShiftL` fromIntegral (b .&. 0x1f))
        OpSlt -> rr (\a b -> if i32FromBits a < i32FromBits b then 1 else 0)
        OpSltu -> rr (\a b -> if a < b then 1 else 0)
        OpXor -> rr xor
        OpSrl -> rr (\a b -> a `unsafeShiftR` fromIntegral (b .&. 0x1f))
        OpSra -> rr (\a b -> arithmeticShiftRight a (fromIntegral (b .&. 0x1f)))
        OpOr -> rr (.|.)
        OpAnd -> rr (.&.)
        OpMul -> rr mulLow
        OpMulh -> rr mulh
        OpMulhsu -> rr mulhsu
        OpMulhu -> rr mulhu
        OpDiv -> rr divSigned
        OpDivu -> rr divUnsigned
        OpRem -> rr remSigned
        OpRemu -> rr remUnsigned
        -- Single-threaded: FENCE has no host-visible memory effect.
        OpFence -> next pn g
        OpEcall -> do
          -- Advance PC and stash a0..a7 before yielding to the host; the
          -- ecall's own gas is already charged.
          copyMutableByteArray regs (ecallSlot * 4) regs (10 * 4) (8 * 4)
          save pn g
          flags <- readState m sFlags
          writeState m sFlags (flags .|. flagPending)
          pure StepEcall
        OpEbreak -> trapAt TrapBreakpoint p g
        OpUnimp -> trapAt TrapInvalidInstruction p g

-- | The op tag of a packed instruction ('packInstruction' stores
-- @fromEnum op@, so the tag is always in range).
opOf :: Word64 -> Op
opOf w = case fromIntegral (w .&. 0xff) of I# t -> tagToEnum# t
{-# INLINE opOf #-}

------------------------------------------------------------------------------
-- Unaligned little-endian guest-memory access. The caller has bounds-checked
-- the address. On a little-endian host these are single unaligned primops.

memRead16 :: MutableByteArray Exts.RealWorld -> Int -> IO Word16
memRead16 a@(MutableByteArray mba) i@(I# i#) = case targetByteOrder of
  LittleEndian -> IO $ \s -> case Exts.readWord8ArrayAsWord16# mba i# s of
    (# s', v #) -> (# s', W16# v #)
  BigEndian -> do
    b0 <- readByteArray a i :: IO Word8
    b1 <- readByteArray a (i + 1) :: IO Word8
    pure (fromIntegral b0 .|. (fromIntegral b1 `shiftL` 8))
{-# INLINE memRead16 #-}

memRead32 :: MutableByteArray Exts.RealWorld -> Int -> IO Word32
memRead32 a@(MutableByteArray mba) i@(I# i#) = case targetByteOrder of
  LittleEndian -> IO $ \s -> case Exts.readWord8ArrayAsWord32# mba i# s of
    (# s', v #) -> (# s', W32# v #)
  BigEndian -> do
    bs <- mapM (\k -> readByteArray a (i + k) :: IO Word8) [0 .. 3]
    pure (foldr (\(k, b) acc -> acc .|. (fromIntegral b `shiftL` (8 * k))) 0 (zip [0 ..] bs))
{-# INLINE memRead32 #-}

memWrite16 :: MutableByteArray Exts.RealWorld -> Int -> Word16 -> IO ()
memWrite16 a@(MutableByteArray mba) i@(I# i#) v@(W16# v#) = case targetByteOrder of
  LittleEndian -> IO $ \s -> (# Exts.writeWord8ArrayAsWord16# mba i# v# s, () #)
  BigEndian -> do
    writeByteArray a i (fromIntegral v :: Word8)
    writeByteArray a (i + 1) (fromIntegral (v `unsafeShiftR` 8) :: Word8)
{-# INLINE memWrite16 #-}

memWrite32 :: MutableByteArray Exts.RealWorld -> Int -> Word32 -> IO ()
memWrite32 a@(MutableByteArray mba) i@(I# i#) v@(W32# v#) = case targetByteOrder of
  LittleEndian -> IO $ \s -> (# Exts.writeWord8ArrayAsWord32# mba i# v# s, () #)
  BigEndian -> mapM_ (\k -> writeByteArray a (i + k) (fromIntegral (v `unsafeShiftR` (8 * k)) :: Word8)) [0 .. 3]
{-# INLINE memWrite32 #-}

------------------------------------------------------------------------------
-- ByteString <-> ByteArray

byteStringToByteArray :: ByteString -> ByteArray
byteStringToByteArray bs = unsafeDupablePerformIO $ do
  let len = B.length bs
  marr <- newByteArray len
  when (len > 0) $
    BU.unsafeUseAsCString bs $ \src ->
      copyPtrToMutableByteArray marr 0 (castPtr src :: Ptr Word8) len
  unsafeFreezeByteArray marr

byteArrayToByteString :: ByteArray -> ByteString
byteArrayToByteString arr =
  let len = sizeofByteArray arr
   in if len == 0
        then B.empty
        else BI.unsafeCreate len (\dst -> copyByteArrayToPtr dst arr 0 len)
