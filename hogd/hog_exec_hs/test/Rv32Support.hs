-- | Test-only builders for RV32 programs and artifacts: ports of
-- @test_program_builder.h@ and @test_artifact_builder.h@, plus a small
-- host-driving harness.
module Rv32Support
  ( -- * Instruction encoders
    rType
  , iType
  , sType
  , bType
  , uType
  , jType
  , addi
  , load
  , jalr
  , op
  , branch
  , store
  , kEcall
  , kEbreak
  , kUnimp
  , kFence
  , codeFromWords
    -- * Artifacts
  , buildArtifact
  , setU32At
  , setU64At
  , setBytesAt
    -- * Machines
  , prepareWords
  , machineFor
  , machineWithPc
  , Host
  , runWithHost
  , haltHost
  , haltWithA0Host
  , continueHost
  , hex
  , unhex
  ) where

import Data.Bits (shiftL, shiftR, (.&.), (.|.))
import qualified Data.ByteString as B
import Data.Char (digitToInt, intToDigit)
import Data.Int (Int32)
import Data.Word (Word32, Word64, Word8)
import Hogd.Exec.Runtime.Rv32.Artifact
import Hogd.Exec.Runtime.Rv32.Encoding (putU32Le, putU64Le)
import Hogd.Exec.Runtime.Rv32.Host
import Hogd.Exec.Runtime.Rv32.Machine
import Hogd.Exec.Runtime.Rv32.Program (ProgramError)

-- Every encoder masks its immediate to the field width (like the C++
-- builders, which additionally abort on immediates that fit neither the
-- signed nor the raw-bits reading; the call sites here never pass those).
requireField :: Bool -> String -> a -> a
requireField ok what x = if ok then x else error ("invalid instruction field: " ++ what)

fitsImm :: Int32 -> Int -> Bool
fitsImm imm b = imm >= negate (1 `shiftL` (b - 1)) && imm < (1 `shiftL` b)

rType :: Word32 -> Word32 -> Word32 -> Word32 -> Word32 -> Word32
rType funct7 rs2 rs1 funct3 rd =
  requireField (funct7 < 0x80 && rs2 < 32 && rs1 < 32 && funct3 < 8 && rd < 32) "r_type" $
    (funct7 `shiftL` 25) .|. (rs2 `shiftL` 20) .|. (rs1 `shiftL` 15) .|. (funct3 `shiftL` 12) .|. (rd `shiftL` 7) .|. 0x33

iType :: Word32 -> Word32 -> Word32 -> Word32 -> Int32 -> Word32
iType opc rd funct3 rs1 imm =
  requireField (opc < 0x80 && rd < 32 && funct3 < 8 && rs1 < 32 && fitsImm imm 12) "i_type" $
    ((fromIntegral imm .&. 0xfff) `shiftL` 20) .|. (rs1 `shiftL` 15) .|. (funct3 `shiftL` 12) .|. (rd `shiftL` 7) .|. opc

sType :: Word32 -> Word32 -> Word32 -> Int32 -> Word32
sType funct3 rs1 rs2 imm =
  requireField (funct3 < 8 && rs1 < 32 && rs2 < 32 && fitsImm imm 12) "s_type" $
    let u = fromIntegral imm .&. 0xfff :: Word32
     in ((u `shiftR` 5) `shiftL` 25) .|. (rs2 `shiftL` 20) .|. (rs1 `shiftL` 15) .|. (funct3 `shiftL` 12) .|. ((u .&. 0x1f) `shiftL` 7) .|. 0x23

bType :: Word32 -> Word32 -> Word32 -> Int32 -> Word32
bType funct3 rs1 rs2 imm =
  requireField (funct3 < 8 && rs1 < 32 && rs2 < 32 && imm .&. 1 == 0 && fitsImm imm 13) "b_type" $
    let u = fromIntegral imm .&. 0x1fff :: Word32
     in (((u `shiftR` 12) .&. 1) `shiftL` 31)
          .|. (((u `shiftR` 5) .&. 0x3f) `shiftL` 25)
          .|. (rs2 `shiftL` 20)
          .|. (rs1 `shiftL` 15)
          .|. (funct3 `shiftL` 12)
          .|. (((u `shiftR` 1) .&. 0xf) `shiftL` 8)
          .|. (((u `shiftR` 11) .&. 1) `shiftL` 7)
          .|. 0x63

uType :: Word32 -> Word32 -> Word32 -> Word32
uType opc rd imm = requireField (opc < 0x80 && rd < 32) "u_type" $ (imm .&. 0xfffff000) .|. (rd `shiftL` 7) .|. opc

jType :: Word32 -> Int32 -> Word32
jType rd imm =
  requireField (rd < 32 && imm .&. 1 == 0 && fitsImm imm 21) "j_type" $
    let u = fromIntegral imm .&. 0x1fffff :: Word32
     in (((u `shiftR` 20) .&. 1) `shiftL` 31)
          .|. (((u `shiftR` 1) .&. 0x3ff) `shiftL` 21)
          .|. (((u `shiftR` 11) .&. 1) `shiftL` 20)
          .|. (((u `shiftR` 12) .&. 0xff) `shiftL` 12)
          .|. (rd `shiftL` 7)
          .|. 0x6f

addi :: Word32 -> Word32 -> Int32 -> Word32
addi rd rs1 = iType 0x13 rd 0 rs1

load :: Word32 -> Word32 -> Word32 -> Int32 -> Word32
load rd funct3 rs1 = iType 0x03 rd funct3 rs1

jalr :: Word32 -> Word32 -> Int32 -> Word32
jalr rd rs1 = iType 0x67 rd 0 rs1

op :: Word32 -> Word32 -> Word32 -> Word32 -> Word32 -> Word32
op rd rs1 rs2 funct3 funct7 = rType funct7 rs2 rs1 funct3 rd

branch :: Word32 -> Word32 -> Word32 -> Int32 -> Word32
branch = bType

-- | @store(rs1, rs2, imm, funct3)@ (C++ argument order).
store :: Word32 -> Word32 -> Int32 -> Word32 -> Word32
store rs1 rs2 imm funct3 = sType funct3 rs1 rs2 imm

kEcall, kEbreak, kUnimp, kFence :: Word32
kEcall = 0x00000073
kEbreak = 0x00100073
kUnimp = 0xc0001073
kFence = 0x0000000f

codeFromWords :: [Word32] -> B.ByteString
codeFromWords = B.concat . map putU32Le

-- | A v5 artifact: header || code || initial_memory, the stored image at
-- data VA @start@, memory_bytes stamped as @start + size@
-- (@test_artifact::build_artifact@).
buildArtifact :: [Word32] -> B.ByteString -> Word32 -> B.ByteString
buildArtifact ws initialMemory start =
  let code = codeFromWords ws
   in B.concat
        [ kArtifactMagic
        , putU32Le kArtifactFormatVersion
        , putU32Le kArtifactHeaderBytes
        , putU64Le kArtifactVmProfileDigest
        , putU32Le 0
        , putU32Le kArtifactEntryAbiVersion
        , putU32Le kArtifactSyscallAbiVersion
        , putU32Le kGasTableVersion
        , putU64Le kGasScheduleDigest
        , putU32Le (start + fromIntegral (B.length initialMemory))
        , putU32Le (fromIntegral (B.length code))
        , putU32Le (fromIntegral (B.length initialMemory))
        , putU32Le start
        , artifactDigest code
        , artifactDigest initialMemory
        , code
        , initialMemory
        ]

setBytesAt :: Int -> B.ByteString -> B.ByteString -> B.ByteString
setBytesAt offset new bytes = B.take offset bytes <> new <> B.drop (offset + B.length new) bytes

setU32At :: Int -> Word32 -> B.ByteString -> B.ByteString
setU32At offset v = setBytesAt offset (putU32Le v)

setU64At :: Int -> Word64 -> B.ByteString -> B.ByteString
setU64At offset v = setBytesAt offset (putU64Le v)

-- | Prepare raw words with an all-zero memory of @memBytes@.
prepareWords :: [Word32] -> Word32 -> Either ProgramError PreparedContract
prepareWords ws memBytes = preInstantiate (codeFromWords ws) B.empty 0 memBytes

machineFor :: [Word32] -> Word32 -> Word64 -> IO Machine
machineFor ws memBytes gas = case prepareWords ws memBytes of
  Left e -> error ("program rejected: " ++ show e)
  Right p -> newMachine p gas

-- | A machine entering at @initialPc@ (the C++ Machine constructor's
-- @initial_pc@).
machineWithPc :: [Word32] -> Word32 -> Word64 -> Word32 -> IO Machine
machineWithPc ws memBytes gas initialPc = do
  m <- machineFor ws memBytes gas
  setPc m initialPc
  pure m

type Host = Machine -> EcallArgs -> IO HostResult

-- | @Machine::run(host)@: drive start / resume until a terminal result.
runWithHost :: Host -> Machine -> IO StepResult
runWithHost host m = start m >>= go
  where
    go StepEcall = do
      Just args <- pendingEcall m
      r <- host m args
      resumeFromEcall m r >>= go
    go r = pure r

haltHost :: Word32 -> Host
haltHost code _ _ = pure (HostHalt code 0)

-- | Halts with the guest's a0 (the conformance tests' @HaltWithA0Host@).
haltWithA0Host :: Host
haltWithA0Host _ args = pure (HostHalt args.a0 0)

continueHost :: Word32 -> Word64 -> Host
continueHost v g _ _ = pure (continueWith v g)

hex :: B.ByteString -> String
hex = concatMap (\b -> [intToDigit (fromIntegral (b `shiftR` 4)), intToDigit (fromIntegral (b .&. 0xf))]) . B.unpack

unhex :: String -> B.ByteString
unhex = B.pack . go
  where
    go (a : b : rest) = (fromIntegral (digitToInt a * 16 + digitToInt b) :: Word8) : go rest
    go _ = []
