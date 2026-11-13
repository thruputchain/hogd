{-# LANGUAGE NoFieldSelectors #-}

-- | Port of @hogd/hog_exec/runtime/rv32/program.h@ / @program.cc@: limits,
-- VM ceilings, the instruction gas schedule and the canonical RV32IM decoder.
--
-- The production deploy gate decodes through @threaded_program.h@
-- (@ThreadedProgram::decode_one@), which classifies instructions separately
-- from @decode_instruction@ so the C++ differential tests can catch decode
-- disagreements. The two decoders accept exactly the same words and produce
-- the same @(op, rd, rs1, rs2, imm, gas)@; this module ports that one
-- accept/reject set and operand mapping ('decodeInstruction'). Where the two
-- C++ entry points differ (error reported for multi-error programs, ceiling
-- checks) the production behaviour is reproduced in
-- "Hogd.Exec.Runtime.Rv32.Machine" ('Hogd.Exec.Runtime.Rv32.Machine.preInstantiate').
module Hogd.Exec.Runtime.Rv32.Program
  ( -- * Sizes, limits and ceilings
    kInstructionBytes
  , kRegisterCount
  , kDefaultMaxMemoryBytes
  , kDefaultMaxCodeBytes
  , kMemoryCeilingBytes
  , kCodeCeilingBytes
  , Rv32Limits (..)
  , defaultLimits
  , ceilingLimits
    -- * Gas
  , kBaseInstructionGas
  , kBranchInstructionGas
  , kDivInstructionGas
  , kDirectJumpGas
  , kIndirectJumpGas
  , kWideLoadGas
  , InstructionGasEntry (..)
  , instructionGasSchedule
  , gasCostForOp
  , kMaxInstructionGas
    -- * Instructions
  , ProgramError (..)
  , InstructionKind (..)
  , Op (..)
  , kOpCount
  , opName
  , branchTaken
  , DecodedInstruction (..)
  , decodeInstruction
    -- * Canonical program
  , Program
  , validateProgram
  , createProgram
  , programInstructions
  , instructionCount
  , programByteSize
  , containsPc
  , instructionAtAlignedPc
  ) where

import Data.ByteString (ByteString)
import qualified Data.ByteString as B
import Data.Int (Int32)
import qualified Data.Vector as V
import Data.Word (Word32, Word64, Word8)
import Hogd.Exec.Runtime.Rv32.Encoding (loadU32Le)
import Data.Bits ((.&.))
import Hogd.Exec.Runtime.Rv32.IsaOps hiding (funct3, funct7)
import qualified Hogd.Exec.Runtime.Rv32.IsaOps as Isa

kInstructionBytes :: Word32
kInstructionBytes = 4

kRegisterCount :: Int
kRegisterCount = 32

-- | The bounds in force when a chain's configuration states none: the
-- protocol defaults (@kDefaultMaxRv32{Memory,Code}Bytes@), restated so the VM
-- does not depend on the XDR constants (the test suite pins them equal, as
-- @rv32_contract.cc@ does with a static_assert).
kDefaultMaxMemoryBytes, kDefaultMaxCodeBytes :: Word32
kDefaultMaxMemoryBytes = 64 * 1024
kDefaultMaxCodeBytes = 256 * 1024

-- | The most this VM provisions and translates: the ceilings on the
-- configured bounds, checked at genesis load.
kMemoryCeilingBytes, kCodeCeilingBytes :: Word32
kMemoryCeilingBytes = 16 * 1024 * 1024
kCodeCeilingBytes = 4 * 1024 * 1024

-- | The bounds an artifact is admitted under: the chain's configuration at
-- attachment, the ceilings when a deployed contract is instantiated.
data Rv32Limits = Rv32Limits
  { maxCodeBytes :: !Word32
  , maxMemoryBytes :: !Word32
  }
  deriving (Eq, Show)

defaultLimits :: Rv32Limits
defaultLimits = Rv32Limits {maxCodeBytes = kDefaultMaxCodeBytes, maxMemoryBytes = kDefaultMaxMemoryBytes}

ceilingLimits :: Rv32Limits
ceilingLimits = Rv32Limits {maxCodeBytes = kCodeCeilingBytes, maxMemoryBytes = kMemoryCeilingBytes}

-- Gas tiers measured on hog10, normalized to baseline dispatch cost; see
-- program.h and gas_calibration/README.md.
kBaseInstructionGas, kBranchInstructionGas, kDivInstructionGas, kDirectJumpGas, kIndirectJumpGas, kWideLoadGas :: Word64
kBaseInstructionGas = 1
kBranchInstructionGas = 2
kDivInstructionGas = 2
kDirectJumpGas = 3
kIndirectJumpGas = 3
kWideLoadGas = 3

-- | Decoder / program rejection reasons (@ProgramError@ minus @kNone@, which
-- the C++ uses only inside success results).
data ProgramError
  = CodeSizeNotAligned
  | CodeTooLarge
  | MemoryTooLarge
  | UnsupportedCompressedInstruction
  | UnsupportedSystemInstruction
  | UnsupportedInstruction
  deriving (Eq, Show, Enum, Bounded)

data InstructionKind
  = KindLui
  | KindAuipc
  | KindJal
  | KindJalr
  | KindBranch
  | KindLoad
  | KindStore
  | KindOpImm
  | KindOp
  | KindFence
  | KindEcall
  | KindEbreak
  | KindUnimp
  deriving (Eq, Show, Enum, Bounded)

-- | Decoded operations, in the C++ @enum class Op@ order. 'fromEnum' is the
-- value folded into the gas-schedule digest.
data Op
  = OpLui
  | OpAuipc
  | OpJal
  | OpJalr
  | OpBeq
  | OpBne
  | OpBlt
  | OpBge
  | OpBltu
  | OpBgeu
  | OpLb
  | OpLh
  | OpLw
  | OpLbu
  | OpLhu
  | OpSb
  | OpSh
  | OpSw
  | OpAddi
  | OpSlti
  | OpSltiu
  | OpXori
  | OpOri
  | OpAndi
  | OpSlli
  | OpSrli
  | OpSrai
  | OpAdd
  | OpSub
  | OpSll
  | OpSlt
  | OpSltu
  | OpXor
  | OpSrl
  | OpSra
  | OpOr
  | OpAnd
  | OpMul
  | OpMulh
  | OpMulhsu
  | OpMulhu
  | OpDiv
  | OpDivu
  | OpRem
  | OpRemu
  | OpFence
  | OpEcall
  | OpEbreak
  | OpUnimp
  deriving (Eq, Ord, Show, Enum, Bounded)

kOpCount :: Int
kOpCount = fromEnum (maxBound :: Op) + 1

-- | The schedule name of an op (the @u8"..."@ literal in
-- @kInstructionGasSchedule@).
opName :: Op -> ByteString
opName = \case
  OpLui -> "LUI"
  OpAuipc -> "AUIPC"
  OpJal -> "JAL"
  OpJalr -> "JALR"
  OpBeq -> "BEQ"
  OpBne -> "BNE"
  OpBlt -> "BLT"
  OpBge -> "BGE"
  OpBltu -> "BLTU"
  OpBgeu -> "BGEU"
  OpLb -> "LB"
  OpLh -> "LH"
  OpLw -> "LW"
  OpLbu -> "LBU"
  OpLhu -> "LHU"
  OpSb -> "SB"
  OpSh -> "SH"
  OpSw -> "SW"
  OpAddi -> "ADDI"
  OpSlti -> "SLTI"
  OpSltiu -> "SLTIU"
  OpXori -> "XORI"
  OpOri -> "ORI"
  OpAndi -> "ANDI"
  OpSlli -> "SLLI"
  OpSrli -> "SRLI"
  OpSrai -> "SRAI"
  OpAdd -> "ADD"
  OpSub -> "SUB"
  OpSll -> "SLL"
  OpSlt -> "SLT"
  OpSltu -> "SLTU"
  OpXor -> "XOR"
  OpSrl -> "SRL"
  OpSra -> "SRA"
  OpOr -> "OR"
  OpAnd -> "AND"
  OpMul -> "MUL"
  OpMulh -> "MULH"
  OpMulhsu -> "MULHSU"
  OpMulhu -> "MULHU"
  OpDiv -> "DIV"
  OpDivu -> "DIVU"
  OpRem -> "REM"
  OpRemu -> "REMU"
  OpFence -> "FENCE"
  OpEcall -> "ECALL"
  OpEbreak -> "EBREAK"
  OpUnimp -> "UNIMP"

-- | Instruction gas. Branches are priced the same taken or not; JAL is a
-- direct jump (3), JALR and the pointer-carrying loads LH/LHU/LW are 3,
-- divide/remainder 2, everything else 1.
gasCostForOp :: Op -> Word64
gasCostForOp = \case
  OpJal -> kDirectJumpGas
  OpJalr -> kIndirectJumpGas
  OpBeq -> kBranchInstructionGas
  OpBne -> kBranchInstructionGas
  OpBlt -> kBranchInstructionGas
  OpBge -> kBranchInstructionGas
  OpBltu -> kBranchInstructionGas
  OpBgeu -> kBranchInstructionGas
  OpLh -> kWideLoadGas
  OpLw -> kWideLoadGas
  OpLhu -> kWideLoadGas
  OpDiv -> kDivInstructionGas
  OpDivu -> kDivInstructionGas
  OpRem -> kDivInstructionGas
  OpRemu -> kDivInstructionGas
  _ -> kBaseInstructionGas

data InstructionGasEntry = InstructionGasEntry
  { gasEntryOp :: !Op
  , gasEntryName :: !ByteString
  , gasEntryCost :: !Word64
  }
  deriving (Eq, Show)

-- | @kInstructionGasSchedule@: one entry per op, in op order.
instructionGasSchedule :: [InstructionGasEntry]
instructionGasSchedule = [InstructionGasEntry op (opName op) (gasCostForOp op) | op <- [minBound .. maxBound]]

-- | The most any single instruction can cost.
kMaxInstructionGas :: Word64
kMaxInstructionGas = maximum (map (.gasEntryCost) instructionGasSchedule)

branchTaken :: Op -> Word32 -> Word32 -> Bool
branchTaken op lhs rhs = case op of
  OpBeq -> lhs == rhs
  OpBne -> lhs /= rhs
  OpBlt -> i32FromBits lhs < i32FromBits rhs
  OpBge -> i32FromBits lhs >= i32FromBits rhs
  OpBltu -> lhs < rhs
  OpBgeu -> lhs >= rhs
  _ -> False

-- | A validated, decoded instruction (@DecodedInstruction@). Register fields
-- are the raw 5-bit fields of the word, whether or not the op uses them.
data DecodedInstruction = DecodedInstruction
  { word :: !Word32
  , kind :: !InstructionKind
  , op :: !Op
  , rd :: !Word8
  , rs1 :: !Word8
  , rs2 :: !Word8
  , funct3 :: !Word8
  , funct7 :: !Word8
  , immediate :: !Int32
  , gasCost :: !Word64
  }
  deriving (Eq, Show)

-- | The canonical decoder (@decode_instruction@). Compressed, atomic,
-- floating-point, vector and general CSR/privileged encodings are rejected.
-- The admitted SYSTEM words are exactly ECALL (0x00000073), EBREAK
-- (0x00100073) and canonical UNIMP (0xc0001073, @csrrw x0, cycle, x0@).
-- FENCE (opcode 0x0f, funct3 0) is admitted with any other field values;
-- FENCE.I (funct3 1) and other MISC-MEM forms are rejected.
decodeInstruction :: Word32 -> Either ProgramError DecodedInstruction
decodeInstruction w
  -- The v1 profile excludes compressed instructions so every PC is a
  -- four-byte index into the immutable decoded instruction array.
  | w .&. 0x3 /= 0x3 = Left UnsupportedCompressedInstruction
  | otherwise = case opcode w of
      0x37 -> ok KindLui OpLui (immU w)
      0x17 -> ok KindAuipc OpAuipc (immU w)
      0x6f -> ok KindJal OpJal (immJ w)
      0x67
        | f3 /= 0 -> bad
        | otherwise -> ok KindJalr OpJalr (immI w)
      0x63 -> case f3 of
        0 -> ok KindBranch OpBeq (immB w)
        1 -> ok KindBranch OpBne (immB w)
        4 -> ok KindBranch OpBlt (immB w)
        5 -> ok KindBranch OpBge (immB w)
        6 -> ok KindBranch OpBltu (immB w)
        7 -> ok KindBranch OpBgeu (immB w)
        _ -> bad
      0x03 -> case f3 of
        0 -> ok KindLoad OpLb (immI w)
        1 -> ok KindLoad OpLh (immI w)
        2 -> ok KindLoad OpLw (immI w)
        4 -> ok KindLoad OpLbu (immI w)
        5 -> ok KindLoad OpLhu (immI w)
        _ -> bad
      0x23 -> case f3 of
        0 -> ok KindStore OpSb (immS w)
        1 -> ok KindStore OpSh (immS w)
        2 -> ok KindStore OpSw (immS w)
        _ -> bad
      0x13 -> case f3 of
        0 -> ok KindOpImm OpAddi (immI w)
        2 -> ok KindOpImm OpSlti (immI w)
        3 -> ok KindOpImm OpSltiu (immI w)
        4 -> ok KindOpImm OpXori (immI w)
        6 -> ok KindOpImm OpOri (immI w)
        7 -> ok KindOpImm OpAndi (immI w)
        -- Shift immediates carry the shift amount in the rs2 field.
        1
          | f7 /= 0 -> bad
          | otherwise -> ok KindOpImm OpSlli (fromIntegral r2)
        5
          | f7 == 0 -> ok KindOpImm OpSrli (fromIntegral r2)
          | f7 == 0x20 -> ok KindOpImm OpSrai (fromIntegral r2)
          | otherwise -> bad
        _ -> bad
      0x33
        | f7 == 0 -> ok KindOp ([OpAdd, OpSll, OpSlt, OpSltu, OpXor, OpSrl, OpOr, OpAnd] !! fromIntegral f3) 0
        | f7 == 0x20 && f3 == 0 -> ok KindOp OpSub 0
        | f7 == 0x20 && f3 == 5 -> ok KindOp OpSra 0
        | f7 == 0x01 -> ok KindOp ([OpMul, OpMulh, OpMulhsu, OpMulhu, OpDiv, OpDivu, OpRem, OpRemu] !! fromIntegral f3) 0
        | otherwise -> bad
      0x0f
        | f3 /= 0 -> bad
        | otherwise -> ok KindFence OpFence 0
      0x73
        | w == 0x00000073 -> ok KindEcall OpEcall 0
        | w == 0x00100073 -> ok KindEbreak OpEbreak 0
        | w == 0xc0001073 -> ok KindUnimp OpUnimp 0
        -- All other CSR, privileged and environment-return forms.
        | otherwise -> Left UnsupportedSystemInstruction
      _ -> bad
  where
    f3 = Isa.funct3 w
    f7 = Isa.funct7 w
    r2 = rs2Field w
    bad = Left UnsupportedInstruction
    ok k o imm =
      Right
        DecodedInstruction
          { word = w
          , kind = k
          , op = o
          , rd = rdField w
          , rs1 = rs1Field w
          , rs2 = r2
          , funct3 = f3
          , funct7 = f7
          , immediate = imm
          , -- Gas is a property of the validated decoded stream.
            gasCost = gasCostForOp o
          }

-- | Immutable decoded RV32IM code for the canonical interpreter (@Program@).
-- Guest memory cannot alias code. The executor's machine uses the packed
-- form built by 'Hogd.Exec.Runtime.Rv32.Machine.preInstantiate'; this type is
-- the inspectable canonical decode.
newtype Program = Program (V.Vector DecodedInstruction)
  deriving (Eq, Show)

-- | @Program::validate@: alignment, then the code ceiling, then the FIRST
-- word that fails to decode (forward scan).
validateProgram :: ByteString -> Either ProgramError ()
validateProgram code
  | B.length code `mod` 4 /= 0 = Left CodeSizeNotAligned
  | B.length code > fromIntegral kCodeCeilingBytes = Left CodeTooLarge
  | otherwise = mapM_ (fmap (const ()) . decodeInstruction . loadU32Le code) [0, 4 .. B.length code - 4]

-- | @Program::create@.
createProgram :: ByteString -> Either ProgramError Program
createProgram code = do
  validateProgram code
  Program . V.fromList <$> mapM (decodeInstruction . loadU32Le code) [0, 4 .. B.length code - 4]

programInstructions :: Program -> V.Vector DecodedInstruction
programInstructions (Program v) = v

instructionCount :: Program -> Int
instructionCount (Program v) = V.length v

programByteSize :: Program -> Int
programByteSize p = instructionCount p * 4

containsPc :: Program -> Word32 -> Bool
containsPc p pc = pc `mod` 4 == 0 && fromIntegral (pc `div` 4) < instructionCount p

instructionAtAlignedPc :: Program -> Word32 -> DecodedInstruction
instructionAtAlignedPc (Program v) pc = v V.! fromIntegral (pc `div` 4)
