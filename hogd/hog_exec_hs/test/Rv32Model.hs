-- | Test-only reference models, written independently of the library's
-- decoder and interpreter:
--
--  * 'specDecode': a mask/match decoder in the style of riscv-opcodes, with
--    its own immediate extraction and gas table;
--  * 'runCanonical': per-instruction charging (the canonical @Machine@), over
--    pure state with semantics in the style of @spec_model.h@ (arithmetic
--    via 'Integer');
--  * 'runThreaded': the production @ThreadedMachine@'s gas accounting --
--    run charging at run heads with @gas_to_run@ suffix sums split at a
--    @max_run_gas@ cap, speculative deficit runs, suffix refunds on memory
--    faults, deficit settlement at run heads, ecalls and traps -- ported
--    from @threaded_machine.h@ (fusion is not modelled: it never changes a
--    slot's @gas_to_run@ or the refund suffix, see @trap_bad_memory@).
module Rv32Model
  ( SInstr (..)
  , specDecode
  , specGas
  , ModelHost
  , Outcome (..)
  , runCanonical
  , runThreaded
  ) where

import Data.Bits (complement, shiftL, shiftR, testBit, xor, (.&.), (.|.))
import qualified Data.ByteString as B
import qualified Data.IntMap.Strict as IM
import Data.Int (Int32)
import qualified Data.Vector as V
import Data.Word (Word32, Word64, Word8)
import Hogd.Exec.Runtime.Rv32.Host
import Hogd.Exec.Runtime.Rv32.Program (Op (..), ProgramError (..))

data SInstr = SInstr
  { sOp :: !Op
  , sRd :: !Int
  , sRs1 :: !Int
  , sRs2 :: !Int
  , sImm :: !Int32
  }
  deriving (Eq, Show)

data Format = FR | FI | FShift | FS | FB | FU | FJ | FNone

-- (mask, match, op, format), from the RISC-V opcode tables for RV32IM plus
-- FENCE and the three admitted SYSTEM words.
opcodeTable :: [(Word32, Word32, Op, Format)]
opcodeTable =
  [ (0x7f, 0x37, OpLui, FU)
  , (0x7f, 0x17, OpAuipc, FU)
  , (0x7f, 0x6f, OpJal, FJ)
  , (0x707f, 0x67, OpJalr, FI)
  , (0x707f, 0x63, OpBeq, FB)
  , (0x707f, 0x1063, OpBne, FB)
  , (0x707f, 0x4063, OpBlt, FB)
  , (0x707f, 0x5063, OpBge, FB)
  , (0x707f, 0x6063, OpBltu, FB)
  , (0x707f, 0x7063, OpBgeu, FB)
  , (0x707f, 0x03, OpLb, FI)
  , (0x707f, 0x1003, OpLh, FI)
  , (0x707f, 0x2003, OpLw, FI)
  , (0x707f, 0x4003, OpLbu, FI)
  , (0x707f, 0x5003, OpLhu, FI)
  , (0x707f, 0x23, OpSb, FS)
  , (0x707f, 0x1023, OpSh, FS)
  , (0x707f, 0x2023, OpSw, FS)
  , (0x707f, 0x13, OpAddi, FI)
  , (0x707f, 0x2013, OpSlti, FI)
  , (0x707f, 0x3013, OpSltiu, FI)
  , (0x707f, 0x4013, OpXori, FI)
  , (0x707f, 0x6013, OpOri, FI)
  , (0x707f, 0x7013, OpAndi, FI)
  , (0xfe00707f, 0x1013, OpSlli, FShift)
  , (0xfe00707f, 0x5013, OpSrli, FShift)
  , (0xfe00707f, 0x40005013, OpSrai, FShift)
  , (0xfe00707f, 0x33, OpAdd, FR)
  , (0xfe00707f, 0x40000033, OpSub, FR)
  , (0xfe00707f, 0x1033, OpSll, FR)
  , (0xfe00707f, 0x2033, OpSlt, FR)
  , (0xfe00707f, 0x3033, OpSltu, FR)
  , (0xfe00707f, 0x4033, OpXor, FR)
  , (0xfe00707f, 0x5033, OpSrl, FR)
  , (0xfe00707f, 0x40005033, OpSra, FR)
  , (0xfe00707f, 0x6033, OpOr, FR)
  , (0xfe00707f, 0x7033, OpAnd, FR)
  , (0xfe00707f, 0x02000033, OpMul, FR)
  , (0xfe00707f, 0x02001033, OpMulh, FR)
  , (0xfe00707f, 0x02002033, OpMulhsu, FR)
  , (0xfe00707f, 0x02003033, OpMulhu, FR)
  , (0xfe00707f, 0x02004033, OpDiv, FR)
  , (0xfe00707f, 0x02005033, OpDivu, FR)
  , (0xfe00707f, 0x02006033, OpRem, FR)
  , (0xfe00707f, 0x02007033, OpRemu, FR)
  , (0x707f, 0x0f, OpFence, FNone)
  , (0xffffffff, 0x00000073, OpEcall, FNone)
  , (0xffffffff, 0x00100073, OpEbreak, FNone)
  , (0xffffffff, 0xc0001073, OpUnimp, FNone)
  ]

-- | Bits @hi..lo@ (inclusive) as an Integer.
fld :: Word32 -> Int -> Int -> Integer
fld w hi lo = toInteger ((w `shiftR` lo) .&. ((1 `shiftL` (hi - lo + 1)) - 1))

-- | Interpret an @n@-bit two's complement value.
sext :: Int -> Integer -> Int32
sext n v = fromInteger (if v >= 2 ^ (n - 1) then v - 2 ^ n else v)

specDecode :: Word32 -> Either ProgramError SInstr
specDecode w
  | w .&. 3 /= 3 = Left UnsupportedCompressedInstruction
  | otherwise = case [(o, f) | (mask, match, o, f) <- opcodeTable, w .&. mask == match] of
      [(o, f)] -> Right (SInstr o rd r1 r2 (imm f))
      [] | w .&. 0x7f == 0x73 -> Left UnsupportedSystemInstruction
      [] -> Left UnsupportedInstruction
      _ -> error "overlapping opcode table"
  where
    rd = fromInteger (fld w 11 7)
    r1 = fromInteger (fld w 19 15)
    r2 = fromInteger (fld w 24 20)
    imm = \case
      FR -> 0
      FNone -> 0
      FI -> sext 12 (fld w 31 20)
      FShift -> fromInteger (fld w 24 20)
      FS -> sext 12 (fld w 31 25 * 32 + fld w 11 7)
      FB -> sext 13 (fld w 31 31 * 4096 + fld w 7 7 * 2048 + fld w 30 25 * 32 + fld w 11 8 * 2)
      FU -> sext 32 (fld w 31 12 * 4096)
      FJ -> sext 21 (fld w 31 31 * 1048576 + fld w 19 12 * 4096 + fld w 20 20 * 2048 + fld w 30 21 * 2)

-- | The pinned gas table (@Rv32GasTest.version_pins_full_cost_table@).
specGas :: Op -> Word64
specGas o
  | o `elem` [OpJal, OpJalr, OpLh, OpLw, OpLhu] = 3
  | o `elem` [OpBeq, OpBne, OpBlt, OpBge, OpBltu, OpBgeu, OpDiv, OpDivu, OpRem, OpRemu] = 2
  | otherwise = 1

------------------------------------------------------------------------------
-- Semantics

data St = St
  { stRegs :: !(IM.IntMap Word32)
  , stMem :: !(IM.IntMap Word8)
  , stMemSize :: !Int
  }

getR :: St -> Int -> Word32
getR st i = if i == 0 then 0 else IM.findWithDefault 0 i (stRegs st)

setR :: Int -> Word32 -> St -> St
setR 0 _ st = st
setR i v st = st {stRegs = IM.insert i v (stRegs st)}

inBounds :: St -> Word32 -> Int -> Bool
inBounds st addr width = toInteger addr + toInteger width <= toInteger (stMemSize st)

loadLe :: St -> Word32 -> Int -> Integer
loadLe st addr width =
  sum [toInteger (IM.findWithDefault 0 (fromIntegral addr + k) (stMem st)) * 256 ^ k | k <- [0 .. width - 1]]

storeLe :: Word32 -> Int -> Word32 -> St -> St
storeLe addr width v st =
  st {stMem = foldr (\k m -> IM.insert (fromIntegral addr + k) (fromIntegral (v `shiftR` (8 * k))) m) (stMem st) [0 .. width - 1]}

signed :: Word32 -> Integer
signed v = if testBit v 31 then toInteger v - 2 ^ (32 :: Int) else toInteger v

wrap :: Integer -> Word32
wrap = fromInteger . (`mod` (2 ^ (32 :: Int)))

-- | Truncating division as in C++ / RISC-V.
tquot, trem :: Integer -> Integer -> Integer
tquot a b = let q = abs a `div` abs b in if (a < 0) /= (b < 0) then negate q else q
trem a b = a - b * tquot a b

data Effect = ENext | EJump !Word32 | ETrap !Trap | EEcall

-- | One instruction's effect at @pc@ (gas is the runners' business).
exec :: Word32 -> SInstr -> Word32 -> St -> (Effect, St)
exec codeBytes i pc st = case sOp i of
  OpLui -> (ENext, setR rd immW st)
  OpAuipc -> (ENext, setR rd (pc + immW) st)
  OpJal -> jumpTo (wrap (toInteger pc + immI)) (setR rd (pc + 4) st)
  OpJalr -> jumpTo (wrap (toInteger a + immI) .&. complement 1) (setR rd (pc + 4) st)
  OpBeq -> br (a == b)
  OpBne -> br (a /= b)
  OpBlt -> br (signed a < signed b)
  OpBge -> br (signed a >= signed b)
  OpBltu -> br (a < b)
  OpBgeu -> br (a >= b)
  OpLb -> ld 1 (\v -> wrap (if v >= 128 then v - 256 else v))
  OpLh -> ld 2 (\v -> wrap (if v >= 32768 then v - 65536 else v))
  OpLw -> ld 4 wrap
  OpLbu -> ld 1 wrap
  OpLhu -> ld 2 wrap
  OpSb -> sto 1
  OpSh -> sto 2
  OpSw -> sto 4
  OpAddi -> alu (wrap (toInteger a + immI))
  OpSlti -> alu (if signed a < immI then 1 else 0)
  OpSltiu -> alu (if a < immW then 1 else 0)
  OpXori -> alu (a `xor` immW)
  OpOri -> alu (a .|. immW)
  OpAndi -> alu (a .&. immW)
  OpSlli -> alu (wrap (toInteger a * 2 ^ (immW .&. 31)))
  OpSrli -> alu (wrap (toInteger a `div` 2 ^ (immW .&. 31)))
  OpSrai -> alu (wrap (signed a `div` 2 ^ (immW .&. 31)))
  OpAdd -> alu (wrap (toInteger a + toInteger b))
  OpSub -> alu (wrap (toInteger a - toInteger b))
  OpSll -> alu (wrap (toInteger a * 2 ^ (b .&. 31)))
  OpSlt -> alu (if signed a < signed b then 1 else 0)
  OpSltu -> alu (if a < b then 1 else 0)
  OpXor -> alu (a `xor` b)
  OpSrl -> alu (wrap (toInteger a `div` 2 ^ (b .&. 31)))
  OpSra -> alu (wrap (signed a `div` 2 ^ (b .&. 31)))
  OpOr -> alu (a .|. b)
  OpAnd -> alu (a .&. b)
  OpMul -> alu (wrap (toInteger a * toInteger b))
  OpMulh -> alu (wrap ((signed a * signed b) `div` 2 ^ (32 :: Int)))
  OpMulhsu -> alu (wrap ((signed a * toInteger b) `div` 2 ^ (32 :: Int)))
  OpMulhu -> alu (wrap ((toInteger a * toInteger b) `div` 2 ^ (32 :: Int)))
  OpDiv
    | b == 0 -> alu 0xffffffff
    | signed a == -(2 ^ (31 :: Int)) && signed b == -1 -> alu a
    | otherwise -> alu (wrap (tquot (signed a) (signed b)))
  OpDivu
    | b == 0 -> alu 0xffffffff
    | otherwise -> alu (wrap (toInteger a `div` toInteger b))
  OpRem
    | b == 0 -> alu a
    | signed a == -(2 ^ (31 :: Int)) && signed b == -1 -> alu 0
    | otherwise -> alu (wrap (trem (signed a) (signed b)))
  OpRemu
    | b == 0 -> alu a
    | otherwise -> alu (wrap (toInteger a `mod` toInteger b))
  OpFence -> (ENext, st)
  OpEcall -> (EEcall, st)
  OpEbreak -> (ETrap TrapBreakpoint, st)
  OpUnimp -> (ETrap TrapInvalidInstruction, st)
  where
    rd = sRd i
    a = getR st (sRs1 i)
    b = getR st (sRs2 i)
    immI = toInteger (sImm i)
    immW = fromIntegral (sImm i) :: Word32
    alu v = (ENext, setR rd v st)
    valid t = t `mod` 4 == 0 && t < codeBytes
    -- Validate before any link write: the link is only kept on success.
    jumpTo t st' = if valid t then (EJump t, st') else (ETrap TrapBadPc, st)
    br taken
      | not taken = (ENext, st)
      | otherwise = let t = wrap (toInteger pc + immI) in if valid t then (EJump t, st) else (ETrap TrapBadPc, st)
    addr = wrap (toInteger a + immI)
    ld width f
      | inBounds st addr width = (ENext, setR rd (f (loadLe st addr width)) st)
      | otherwise = (ETrap TrapBadMemory, st)
    sto width
      | inBounds st addr width = (ENext, storeLe addr width b st)
      | otherwise = (ETrap TrapBadMemory, st)

------------------------------------------------------------------------------
-- Runners

-- | Host for model runs: given the 0-based ecall index and arguments,
-- guest-memory writes to make (applied if in bounds) and the result.
type ModelHost = Int -> EcallArgs -> ([(Word32, B.ByteString)], HostResult)

data Outcome = Outcome
  { oResult :: StepResult
  , oTrap :: Trap
  , oGas :: Word64
  , oPc :: Word32
  , oHaltCode :: Word32
  , oEcalls :: [(EcallArgs, Word64)]
  -- ^ Each ecall's arguments and the gas remaining when it reached the host.
  , oRegs :: [Word32]
  , oMem :: B.ByteString
  }
  deriving (Eq, Show)

initialState :: Int -> B.ByteString -> [(Int, Word32)] -> St
initialState memSize image regs =
  St
    { stRegs = IM.fromList [(r, v) | (r, v) <- regs, r /= 0]
    , stMem = IM.fromList (zip [0 ..] (B.unpack image))
    , stMemSize = memSize
    }

ecallArgs :: St -> EcallArgs
ecallArgs st = EcallArgs (r 10) (r 11) (r 12) (r 13) (r 14) (r 15) (r 16) (r 17) where r = getR st

applyWrites :: [(Word32, B.ByteString)] -> St -> St
applyWrites ws st0 = foldl apply st0 ws
  where
    apply st (addr, bytes)
      | memoryRangeInBounds (fromIntegral (stMemSize st)) addr (fromIntegral (B.length bytes)) =
          st {stMem = foldl (\m (k, v) -> IM.insert (fromIntegral addr + k) v m) (stMem st) (zip [0 ..] (B.unpack bytes))}
      | otherwise = st

finish :: StepResult -> Trap -> Word64 -> Word32 -> Word32 -> [(EcallArgs, Word64)] -> St -> Outcome
finish r t g p hc ecalls st =
  Outcome
    { oResult = r
    , oTrap = t
    , oGas = g
    , oPc = p
    , oHaltCode = hc
    , oEcalls = reverse ecalls
    , oRegs = map (getR st) [0 .. 31]
    , oMem = B.pack [IM.findWithDefault 0 k (stMem st) | k <- [0 .. stMemSize st - 1]]
    }

-- | Applies a host result after the host debit check; shared by both
-- runners (@complete_ecall@ / @apply_ecall@ are identical here).
hostResume ::
  HostResult -> Word64 -> St -> (Word64 -> St -> Outcome) -> (Trap -> Word64 -> Outcome) -> (Word32 -> Word64 -> Outcome) -> Outcome
hostResume hr gas st continue' trapK haltK
  | gas < hostGasConsumed hr = trapK TrapOutOfGas 0
  | otherwise =
      let g = gas - hostGasConsumed hr
       in case hr of
            HostContinue v _ -> continue' g (setR 10 v st)
            HostHalt c _ -> haltK c g
            HostOutOfGas _ -> trapK TrapOutOfGas 0

-- | Canonical per-instruction execution.
runCanonical :: ModelHost -> V.Vector SInstr -> Int -> B.ByteString -> [(Int, Word32)] -> Word32 -> Word64 -> Outcome
runCanonical host code memSize image regs pc0 gas0 = go 0 [] pc0 gas0 (initialState memSize image regs)
  where
    codeBytes = fromIntegral (V.length code * 4) :: Word32
    go :: Int -> [(EcallArgs, Word64)] -> Word32 -> Word64 -> St -> Outcome
    go nEcall ecalls pc gas st
      | pc `mod` 4 /= 0 || pc >= codeBytes = finish StepTrap TrapBadPc gas pc 0 ecalls st
      | otherwise =
          let i = code V.! fromIntegral (pc `div` 4)
              c = specGas (sOp i)
           in if gas < c
                then finish StepTrap TrapOutOfGas 0 pc 0 ecalls st
                else
                  let g = gas - c
                   in case exec codeBytes i pc st of
                        (ENext, st') -> go nEcall ecalls (pc + 4) g st'
                        (EJump t, st') -> go nEcall ecalls t g st'
                        (ETrap t, st') -> finish StepTrap t g pc 0 ecalls st'
                        (EEcall, st') ->
                          let args = ecallArgs st'
                              (writes, hr) = host nEcall args
                              st'' = applyWrites writes st'
                              ecalls' = (args, g) : ecalls
                              pc' = pc + 4
                           in hostResume
                                hr
                                g
                                st''
                                (go (nEcall + 1) ecalls' pc')
                                (\t g' -> finish StepTrap t g' pc' 0 ecalls' st'')
                                (\hc g' -> finish StepHalted TrapNone g' pc' hc ecalls' st'')

isBarrier :: Op -> Bool
isBarrier o = o `elem` [OpBeq, OpBne, OpBlt, OpBge, OpBltu, OpBgeu, OpJal, OpJalr, OpEcall, OpEbreak, OpUnimp]

-- | The production engine's gas accounting (see the module header). Trap
-- PCs, and registers/memory after an out-of-gas trap, are this model's own
-- (production reports the run head / speculative state there); callers
-- compare those only for non-out-of-gas outcomes.
runThreaded :: Word64 -> ModelHost -> V.Vector SInstr -> Int -> B.ByteString -> [(Int, Word32)] -> Word64 -> Outcome
runThreaded maxRunGas0 host code memSize image regs gas0 =
  enterRun 0 [] 0 gas0 0 (initialState memSize image regs)
  where
    -- create(): clamp the cap to [kMaxInstructionGas, kMaxRunGas].
    maxRunGas = max 3 (min 10000 maxRunGas0)
    n = V.length code
    codeBytes = fromIntegral (n * 4) :: Word32
    -- Backward pass: gas_to_run (suffix through the next barrier or chunk
    -- break) and break carriers. Slot n is the zero-gas fall-off sentinel.
    (gasToRun, isBreak) =
      let step' i (gs, bs) =
            let o = sOp (code V.! i)
                self = specGas o
                nextG = head gs
                cand = self + nextG
                (g, brk)
                  | not (isBarrier o) && i + 1 < n = if cand <= maxRunGas then (cand, False) else (self, True)
                  | otherwise = (self, False)
             in (g : gs, brk : bs)
          (gs', bs') = foldr step' ([0], [False]) [0 .. n - 1]
       in (V.fromList gs', V.fromList bs')

    trapAt t slot gas deficit ecalls st
      | deficit /= 0 = finish StepTrap TrapOutOfGas 0 (fromIntegral slot * 4) 0 ecalls st
      | otherwise = finish StepTrap t gas (fromIntegral slot * 4) 0 ecalls st

    -- enter_run / enter_run_deficit.
    enterRun nE ecalls slot gas deficit st
      | gas < runGas =
          if deficit /= 0
            then trapAt TrapOutOfGas slot 0 deficit ecalls st
            else body nE ecalls slot 0 (runGas - gas) st
      | otherwise = body nE ecalls slot (gas - runGas) deficit st
      where
        runGas = gasToRun V.! slot

    body nE ecalls slot gas deficit st
      | slot == n = trapAt TrapBadPc slot gas deficit ecalls st
      | otherwise =
          let i = code V.! slot
              pc = fromIntegral slot * 4
           in case exec codeBytes i pc st of
                (ENext, st')
                  | isBreak V.! slot -> enterRun nE ecalls (slot + 1) gas deficit st'
                  -- A not-taken branch ends its run: enter the fall-through.
                  | isBarrier (sOp i) -> enterRun nE ecalls (slot + 1) gas deficit st'
                  | otherwise -> body nE ecalls (slot + 1) gas deficit st'
                (EJump t, st') -> enterRun nE ecalls (fromIntegral (t `div` 4)) gas deficit st'
                (ETrap TrapBadMemory, st') ->
                  -- trap_bad_memory: refund the charged suffix.
                  let suffix = if isBreak V.! slot then 0 else gasToRun V.! (slot + 1)
                      balance = gas + suffix
                   in if balance < deficit
                        then trapAt TrapOutOfGas slot 0 deficit ecalls st'
                        else trapAt TrapBadMemory slot (balance - deficit) 0 ecalls st'
                (ETrap t, st') -> trapAt t slot gas deficit ecalls st'
                (EEcall, st')
                  -- A deficit run settles before the host boundary.
                  | deficit /= 0 -> trapAt TrapOutOfGas slot 0 deficit ecalls st'
                  | otherwise ->
                      let args = ecallArgs st'
                          (writes, hr) = host nE args
                          st'' = applyWrites writes st'
                          ecalls' = (args, gas) : ecalls
                          pc' = pc + 4
                       in hostResume
                            hr
                            gas
                            st''
                            (\g s -> enterRun (nE + 1) ecalls' (slot + 1) g 0 s)
                            (\t g' -> finish StepTrap t g' pc' 0 ecalls' st'')
                            (\hc g' -> finish StepHalted TrapNone g' pc' hc ecalls' st'')
