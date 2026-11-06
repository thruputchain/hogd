-- | RV32 VM tests: ports of the meaningful parts of
-- @hogd/hog_exec/runtime/rv32/*_test.cc@ (artifact, decoder, ISA
-- conformance, borrowed riscv-tests vectors, gas/trap parity, yield API,
-- compiled fixture) plus differential properties against the independent
-- models in "Rv32Model".
module Rv32Spec (spec) where

import Control.Monad (forM_, unless, when)
import qualified Crypto.Hash as H
import qualified Data.ByteArray as BA
import Data.Bits (shiftR, xor, (.&.))
import qualified Data.ByteString as B

import Data.Either (isRight)
import Data.IORef
import Data.Int (Int32)
import Data.List (nub)
import qualified Data.Vector as V
import Data.Word (Word32, Word64, Word8)
import Hogd.Exec.Runtime.GasCostTable
import Hogd.Exec.Runtime.Rv32.Artifact
import Hogd.Exec.Runtime.Rv32.Encoding (loadU32Le)
import Hogd.Exec.Runtime.Rv32.Host
import Hogd.Exec.Runtime.Rv32.Machine
import Hogd.Exec.Runtime.Rv32.Program
import Hogd.Exec.Runtime.Rv32.Rv32Contract
import Hogd.Xdr.DbConflict
import Hogd.Xdr.HogExecConstants (kDefaultMaxRv32CodeBytes, kDefaultMaxRv32MemoryBytes)
import qualified Rv32Fixture as Fx
import Rv32Model
import Rv32Support
import Rv32Vectors
import Test.Hspec
import Test.QuickCheck hiding ((.&.))

spec :: Spec
spec = do
  constantsSpec
  artifactSpec
  contractSpec
  decoderSpec
  executionSpec
  borrowedSpec
  yieldSpec
  gasSpec
  fixtureSpec
  differentialSpec

------------------------------------------------------------------------------
-- Helpers

validArtifact :: B.ByteString
validArtifact = buildArtifact [addi 0 0 0, kEcall] (B.pack [1, 2, 3, 4]) 0

parseErr :: Rv32Limits -> B.ByteString -> Maybe ArtifactError
parseErr limits = either Just (const Nothing) . parseArtifact limits

createErr :: Rv32Limits -> B.ByteString -> Maybe ArtifactError
createErr limits = either Just (const Nothing) . createContract limits

expectArtifactError :: B.ByteString -> ArtifactError -> Expectation
expectArtifactError bytes e = do
  parseErr defaultLimits bytes `shouldBe` Just e
  createErr defaultLimits bytes `shouldBe` Just e

regs :: Machine -> IO [Word32]
regs m = mapM (reg m) [0 .. 31]

-- | Run to a terminal result and gather the observable outcome, using a
-- model host (memory writes go through 'writeGuest').
runOutcome :: ModelHost -> Machine -> IO Outcome
runOutcome host m = do
  ecallsRef <- newIORef []
  let go i StepEcall = do
        Just args <- pendingEcall m
        g <- gasRemaining m
        modifyIORef' ecallsRef ((args, g) :)
        let (writes, hr) = host i args
        mapM_ (uncurry (writeGuest m)) writes
        resumeFromEcall m hr >>= go (i + 1)
      go _ r = pure r
  r <- start m >>= go 0
  Outcome r
    <$> trap m
    <*> gasRemaining m
    <*> pc m
    <*> haltCode m
    <*> (reverse <$> readIORef ecallsRef)
    <*> regs m
    <*> memorySnapshot m

sha256 :: B.ByteString -> B.ByteString
sha256 = BA.convert . H.hashWith H.SHA256

------------------------------------------------------------------------------

constantsSpec :: Spec
constantsSpec = describe "constants and schedules" $ do
  it "pins the instruction gas table (Rv32GasTest.version_pins_full_cost_table)" $ do
    kOpCount `shouldBe` 49
    kGasTableVersion `shouldBe` 4
    let expected =
          [ ("LUI", 1), ("AUIPC", 1), ("JAL", 3), ("JALR", 3), ("BEQ", 2), ("BNE", 2), ("BLT", 2), ("BGE", 2)
          , ("BLTU", 2), ("BGEU", 2), ("LB", 1), ("LH", 3), ("LW", 3), ("LBU", 1), ("LHU", 3), ("SB", 1)
          , ("SH", 1), ("SW", 1), ("ADDI", 1), ("SLTI", 1), ("SLTIU", 1), ("XORI", 1), ("ORI", 1), ("ANDI", 1)
          , ("SLLI", 1), ("SRLI", 1), ("SRAI", 1), ("ADD", 1), ("SUB", 1), ("SLL", 1), ("SLT", 1), ("SLTU", 1)
          , ("XOR", 1), ("SRL", 1), ("SRA", 1), ("OR", 1), ("AND", 1), ("MUL", 1), ("MULH", 1), ("MULHSU", 1)
          , ("MULHU", 1), ("DIV", 2), ("DIVU", 2), ("REM", 2), ("REMU", 2), ("FENCE", 1), ("ECALL", 1)
          , ("EBREAK", 1), ("UNIMP", 1)
          ] ::
            [(B.ByteString, Word64)]
    map (\e -> (e.gasEntryName, e.gasEntryCost)) instructionGasSchedule `shouldBe` expected
    map (\e -> fromEnum e.gasEntryOp) instructionGasSchedule `shouldBe` [0 .. 48]
    kMaxInstructionGas `shouldBe` 3
  it "pins the host syscall schedule" $
    map (\e -> (syscallGasName e, syscallGasCost e)) syscallGasCostSchedule
      `shouldBe` [ ("SyscallBase", 50), ("SHA256HashBlock", 22), ("SHA256Stream", 26), ("VerifySignatureBase", 17750)
                 , ("VerifySignatureBlock", 131), ("StorageAccess", 150), ("DBRPC", 37300), ("BytestringBlockBytes", 512)
                 , ("BytestringWriteBlock", 13), ("BytestringReadBlock", 7), ("NoSyscall", 10), ("GenericBadSyscall", 10)
                 , ("VerifySignatureMlDsa65Base", 46592), ("VerifySignatureMlDsa65Block", 160)
                 ]
  it "computes the pinned gas schedule digest" $ do
    gasScheduleDigest `shouldBe` 0xa958d226df658598
    kGasScheduleDigest `shouldBe` gasScheduleDigest
  it "accepts exactly the current and transitional gas schedules" $
    kAcceptedGasSchedules
      `shouldBe` [AcceptedGasSchedule 4 0xa958d226df658598, AcceptedGasSchedule 3 0x918317c21c46bb60]
  it "computes the pinned format fingerprints" $ do
    kArtifactFormatFingerprint `shouldBe` 0x4171b4b8dc19b9ab
    kAcceptedFormatFingerprints `shouldBe` [0x4171b4b8dc19b9ab, 0x8e5caf3e9cd23b22, 0xc5f00a7d4451045e]
    formatFingerprint (kArtifactFormatVersion + 1) kGasTableVersion kGasScheduleDigest False
      `shouldNotBe` kArtifactFormatFingerprint
  it "accepts the committed fixture lowerer's fingerprint" $
    Fx.kLowererFormatFingerprint `elem` kAcceptedFormatFingerprints `shouldBe` True
  it "restates the protocol's default limits" $ do
    kDefaultMaxCodeBytes `shouldBe` kDefaultMaxRv32CodeBytes
    kDefaultMaxMemoryBytes `shouldBe` kDefaultMaxRv32MemoryBytes
    kDefaultMaxMemoryBytes <= kMemoryCeilingBytes `shouldBe` True
    kDefaultMaxCodeBytes <= kCodeCeilingBytes `shouldBe` True
  it "computes syscall cost functions" $ do
    sha256Hash 7 `shouldBe` 22
    sha256Hash 55 `shouldBe` 22
    sha256Hash 56 `shouldBe` 44
    sha256Update 63 `shouldBe` 26
    sha256Update 128 `shouldBe` 26 + 44
    bytestringSet 0 `shouldBe` 150
    bytestringGet 513 `shouldBe` 150 + 14
    verifySignature 0 `shouldBe` 17750 + 131
    verifySignatureMlDsa65 0 `shouldBe` 46592 + 160

------------------------------------------------------------------------------

artifactSpec :: Spec
artifactSpec = describe "artifact parsing" $ do
  it "computes canonical SHA-256 section digests" $ do
    hex (artifactDigest B.empty) `shouldBe` "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    hex (artifactDigest "abc") `shouldBe` "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    hex (artifactDigest "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
      `shouldBe` "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
  it "parses the compiled fixture artifact" $ do
    B.length Fx.kArtifact `shouldBe` fromIntegral Fx.kArtifactBytes
    hex (sha256 Fx.kArtifact) `shouldBe` Fx.kArtifactSha256Hex
    case parseArtifact defaultLimits Fx.kArtifact of
      Left e -> expectationFailure (show e)
      Right a -> do
        B.length (artifactCode a) `shouldBe` fromIntegral Fx.kCodeBytes
        artifactMemoryBytes a `shouldBe` Fx.kMemoryBytes
        artifactInitialMemoryStart a `shouldBe` Fx.kInitialMemoryStart
        artifactInitialMemory a `shouldBe` Fx.kInput
        Fx.kInitialMemoryStart + Fx.kInitialMemoryBytes <= Fx.kMemoryBytes `shouldBe` True
  it "rejects header, version and profile mismatches" $ do
    expectArtifactError (setBytesAt 0 (B.pack [0x47 `xor` 0xff]) validArtifact) ArtifactBadMagic
    expectArtifactError (setU32At kArtifactFormatVersionOffset 6 validArtifact) ArtifactUnsupportedFormatVersion
    expectArtifactError (setU32At kArtifactHeaderSizeOffset 132 validArtifact) ArtifactBadHeaderSize
    expectArtifactError (setU64At kArtifactVmProfileOffset 0 validArtifact) ArtifactUnsupportedVmProfile
    expectArtifactError (setU32At kArtifactFlagsOffset 1 validArtifact) ArtifactUnsupportedFlags
  it "rejects ABI and gas mismatches" $ do
    expectArtifactError (setU32At kArtifactEntryAbiOffset 4 validArtifact) ArtifactUnsupportedEntryAbi
    expectArtifactError (setU32At kArtifactSyscallAbiOffset 2 validArtifact) ArtifactUnsupportedSyscallAbi
    -- The pre-register-native syscall ABI (0) is rejected too.
    expectArtifactError (setU32At kArtifactSyscallAbiOffset 0 validArtifact) ArtifactUnsupportedSyscallAbi
    expectArtifactError (setU32At kArtifactGasVersionOffset 5 validArtifact) ArtifactGasScheduleVersionMismatch
    expectArtifactError (setU64At kArtifactGasDigestOffset (kGasScheduleDigest `xor` 1) validArtifact) ArtifactGasScheduleDigestMismatch
  it "accepts only registered gas schedule pairs" $ do
    let transitional =
          setU64At kArtifactGasDigestOffset kTransitionalGasScheduleDigest $
            setU32At kArtifactGasVersionOffset kTransitionalGasTableVersion validArtifact
    parseArtifact defaultLimits transitional `shouldSatisfy` isRight
    createContract defaultLimits transitional `shouldSatisfy` isRight
    expectArtifactError (setU64At kArtifactGasDigestOffset kTransitionalGasScheduleDigest validArtifact) ArtifactGasScheduleDigestMismatch
    expectArtifactError (setU32At kArtifactGasVersionOffset kTransitionalGasTableVersion validArtifact) ArtifactGasScheduleDigestMismatch
  it "rejects extreme header integers without wrapping" $ do
    forM_
      [ (kArtifactMemoryBytesOffset, ArtifactMemorySizeInvalid)
      , (kArtifactCodeSizeOffset, ArtifactCodeSizeInvalid)
      , (kArtifactInitialMemorySizeOffset, ArtifactInitialMemoryTooLarge)
      , (kArtifactInitialMemoryStartOffset, ArtifactInitialMemoryTooLarge)
      ]
      $ \(offset, e) -> expectArtifactError (setU32At offset maxBound validArtifact) e
    -- With permissive limits start+size would wrap in uint32; the
    -- subtraction-form check still rejects the placement.
    let wrapping = setU32At kArtifactInitialMemoryStartOffset (maxBound - 1) (setU32At kArtifactMemoryBytesOffset maxBound validArtifact)
        permissive = Rv32Limits maxBound maxBound
    parseErr permissive wrapping `shouldBe` Just ArtifactInitialMemoryTooLarge
  it "honors zero and exact caller limits" $ do
    let codeSize = loadU32Le validArtifact kArtifactCodeSizeOffset
        memSize = loadU32Le validArtifact kArtifactMemoryBytesOffset
    createErr (Rv32Limits 0 memSize) validArtifact `shouldBe` Just ArtifactCodeSizeInvalid
    createErr (Rv32Limits (codeSize - 1) memSize) validArtifact `shouldBe` Just ArtifactCodeSizeInvalid
    createErr (Rv32Limits codeSize (memSize - 1)) validArtifact `shouldBe` Just ArtifactMemorySizeInvalid
    createErr (Rv32Limits codeSize memSize) validArtifact `shouldBe` Nothing
    let zeroMemory = buildArtifact [addi 0 0 0] B.empty 0
    createErr (Rv32Limits 4 0) zeroMemory `shouldBe` Nothing
  it "takes bounds from the caller's limits" $ do
    let raised = Rv32Limits (2 * kDefaultMaxCodeBytes) (2 * kDefaultMaxMemoryBytes)
        bigCode = buildArtifact (replicate (fromIntegral (kDefaultMaxCodeBytes `div` 4) + 1) (addi 0 0 0)) B.empty 0
    createErr defaultLimits bigCode `shouldBe` Just ArtifactCodeSizeInvalid
    case createContract raised bigCode of
      Left e -> expectationFailure (show e)
      Right c -> preparedInstructionCount (contractPrepared c) `shouldBe` fromIntegral (kDefaultMaxCodeBytes `div` 4) + 1
    let bigMemory = setU32At kArtifactMemoryBytesOffset (kDefaultMaxMemoryBytes + 1) validArtifact
    createErr defaultLimits bigMemory `shouldBe` Just ArtifactMemorySizeInvalid
    (artifactMemoryBytes <$> parseArtifact raised bigMemory) `shouldBe` Right (kDefaultMaxMemoryBytes + 1)
    let atCeiling = setU32At kArtifactMemoryBytesOffset kMemoryCeilingBytes validArtifact
    createErr ceilingLimits atCeiling `shouldBe` Nothing
  it "rejects bounds and section layout errors" $ do
    expectArtifactError (setU32At kArtifactMemoryBytesOffset (kDefaultMaxMemoryBytes + 1) validArtifact) ArtifactMemorySizeInvalid
    expectArtifactError (setU32At kArtifactCodeSizeOffset 0 validArtifact) ArtifactCodeSizeInvalid
    expectArtifactError (setU32At kArtifactCodeSizeOffset (kDefaultMaxCodeBytes + 1) validArtifact) ArtifactCodeSizeInvalid
    expectArtifactError (setU32At kArtifactInitialMemorySizeOffset (kDefaultMaxMemoryBytes + 1) validArtifact) ArtifactInitialMemoryTooLarge
    -- initial_memory_start pushes the image past memory_bytes.
    expectArtifactError (setU32At kArtifactInitialMemoryStartOffset 1 validArtifact) ArtifactInitialMemoryTooLarge
    -- Canonicality: an empty image must have start == 0.
    let emptyImage = buildArtifact [addi 0 0 0, kEcall] B.empty 0
    expectArtifactError (setU32At kArtifactInitialMemoryStartOffset 4 (setU32At kArtifactMemoryBytesOffset 64 emptyImage)) ArtifactInitialMemoryTooLarge
    expectArtifactError (validArtifact <> B.singleton 0) ArtifactInvalidSectionLayout
    expectArtifactError (B.init validArtifact) ArtifactInvalidSectionLayout
    expectArtifactError (setU32At kArtifactCodeSizeOffset 12 validArtifact) ArtifactInvalidSectionLayout
  it "rejects digest and program errors" $ do
    expectArtifactError (setBytesAt kArtifactCodeDigestOffset (B.replicate 32 0) validArtifact) ArtifactCodeDigestMismatch
    expectArtifactError (setBytesAt kArtifactInitialMemoryDigestOffset (B.replicate 32 0) validArtifact) ArtifactInitialMemoryDigestMismatch
    let zeroed = setBytesAt 128 (B.singleton 0) validArtifact
        rehashed = setBytesAt kArtifactCodeDigestOffset (artifactDigest (B.take 8 (B.drop 128 zeroed))) zeroed
    parseErr defaultLimits rehashed `shouldBe` Nothing
    createErr defaultLimits rehashed `shouldBe` Just ArtifactInvalidProgram
    expectArtifactError (B.take 127 validArtifact) ArtifactTooSmall
  it "reports the first failing check" $ do
    -- Bad magic and bad version: magic wins; bad digests and bad layout:
    -- layout wins.
    expectArtifactError (setBytesAt 0 "X" (setU32At kArtifactFormatVersionOffset 9 validArtifact)) ArtifactBadMagic
    expectArtifactError (setBytesAt kArtifactCodeDigestOffset (B.replicate 32 0) validArtifact <> "x") ArtifactInvalidSectionLayout
  it "accepts a zero-extended initial memory prefix" $ do
    let a = setU32At kArtifactMemoryBytesOffset 256 validArtifact
    (artifactMemoryBytes <$> parseArtifact defaultLimits a) `shouldBe` Right 256
    case createContract defaultLimits a of
      Left e -> expectationFailure (show e)
      Right c -> preparedInitialMemory (contractPrepared c) `shouldBe` B.pack [1, 2, 3, 4] <> B.replicate 252 0
  it "accepts a zero-memory artifact whose data accesses all fault" $ do
    let a = buildArtifact [addi 1 0 7, kEbreak] B.empty 0
    (artifactMemoryBytes <$> parseArtifact defaultLimits a) `shouldBe` Right 0
    case createContract defaultLimits a of
      Left e -> expectationFailure (show e)
      Right c -> do
        m <- instantiateContract c 0 100
        start m `shouldReturn` StepTrap
        trap m `shouldReturn` TrapBreakpoint
        reg m 1 `shouldReturn` 7
        memorySize m `shouldBe` 0
        readGuest m 0 0 `shouldReturn` Just B.empty
        readGuest m 0 1 `shouldReturn` Nothing
  it "accepts limit boundary values" $ do
    let a = setU32At kArtifactMemoryBytesOffset kDefaultMaxMemoryBytes validArtifact
    createErr defaultLimits a `shouldBe` Nothing
    let full = buildArtifact (replicate (fromIntegral (kDefaultMaxCodeBytes `div` 4)) (addi 0 0 0)) (B.singleton 1) 0
    createErr defaultLimits full `shouldBe` Nothing
  it "materializes initial memory at the start offset" $ do
    let a = buildArtifact [addi 0 0 0, kEcall] (B.pack [1, 2, 3, 4]) 8
    case parseArtifact defaultLimits a of
      Left e -> expectationFailure (show e)
      Right p -> do
        artifactInitialMemoryStart p `shouldBe` 8
        artifactMemoryBytes p `shouldBe` 12
        artifactInitialMemory p `shouldBe` B.pack [1, 2, 3, 4]
    case createContract defaultLimits a of
      Left e -> expectationFailure (show e)
      Right c -> do
        m <- instantiateContract c 0 10
        memorySnapshot m `shouldReturn` (B.replicate 8 0 <> B.pack [1, 2, 3, 4])

------------------------------------------------------------------------------

unsupportedProfile :: [(String, Word32, ProgramError)]
unsupportedProfile =
  [ ("lr.w", 0x100120af, UnsupportedInstruction)
  , ("amoadd.w", 0x004121af, UnsupportedInstruction)
  , ("c.nop", 0x00000001, UnsupportedCompressedInstruction)
  , ("fence.i", 0x0000100f, UnsupportedInstruction)
  , ("fadd.s", 0x0020f1d3, UnsupportedInstruction)
  , ("fadd.d", 0x0220f1d3, UnsupportedInstruction)
  , ("fadd.h", 0x0420f1d3, UnsupportedInstruction)
  , ("clmul", 0x0a2091b3, UnsupportedInstruction)
  , ("sh1add", 0x2020a1b3, UnsupportedInstruction)
  , ("andn", 0x4020f1b3, UnsupportedInstruction)
  , ("pack", 0x0820c1b3, UnsupportedInstruction)
  , ("xperm4", 0x2820a1b3, UnsupportedInstruction)
  , ("bset", 0x282091b3, UnsupportedInstruction)
  , ("czero.eqz", 0x0e20d1b3, UnsupportedInstruction)
  , ("wfi", 0x10500073, UnsupportedSystemInstruction)
  , ("csrrw", 0x001110f3, UnsupportedSystemInstruction)
  , ("mret", 0x30200073, UnsupportedSystemInstruction)
  ]

contractSpec :: Spec
contractSpec = describe "contract preparation" $ do
  it "defers misaligned code to preparation (InvalidProgram)" $
    forM_ [1, 2, 3] $ \n -> do
      let code = B.take n (codeFromWords [addi 0 0 0, kEcall])
          a0 =
            setBytesAt kArtifactCodeDigestOffset (artifactDigest code) $
              setU32At kArtifactCodeSizeOffset (fromIntegral n) $
                B.take 128 validArtifact <> code <> B.pack [1, 2, 3, 4]
      parseErr defaultLimits a0 `shouldBe` Nothing
      createErr defaultLimits a0 `shouldBe` Just ArtifactInvalidProgram
      either Just (const Nothing) (preInstantiate code B.empty 0 0) `shouldBe` Just CodeSizeNotAligned
  it "rejects the unsupported-profile table at the program and artifact boundary" $
    forM_ unsupportedProfile $ \(name, w, e) -> do
      (name, validateProgram (codeFromWords [w])) `shouldBe` (name, Left e)
      (name, either Just (const Nothing) (preInstantiate (codeFromWords [w]) B.empty 0 16)) `shouldBe` (name, Just e)
      (name, createErr defaultLimits (buildArtifact [w] (B.replicate 16 0) 0)) `shouldBe` (name, Just ArtifactInvalidProgram)
  it "reports the first decode error canonically and the last in production" $ do
    let check ws firstErr lastErr = do
          validateProgram (codeFromWords ws) `shouldBe` Left firstErr
          either Just (const Nothing) (preInstantiate (codeFromWords ws) B.empty 0 256) `shouldBe` Just lastErr
    check [0xffffffff, addi 1 0 1, 0x00200073] UnsupportedInstruction UnsupportedSystemInstruction
    check [0x00200073, addi 1 0 1, 0xffffffff] UnsupportedSystemInstruction UnsupportedInstruction
    check [0x00000000, 0xffffffff, 0x00200073] UnsupportedCompressedInstruction UnsupportedSystemInstruction
  it "applies the VM ceilings at preparation" $ do
    let ok = isRight . (\(c, mem, s, t) -> preInstantiate c mem s t)
    ok (codeFromWords [addi 0 0 0], B.empty, 0, kMemoryCeilingBytes) `shouldBe` True
    preInstantiate (codeFromWords [addi 0 0 0]) B.empty 0 (kMemoryCeilingBytes + 1) `shouldSatisfy` (== Just MemoryTooLarge) . either Just (const Nothing)
    preInstantiate (codeFromWords [addi 0 0 0]) (B.pack [1, 2, 3, 4]) 4 7 `shouldSatisfy` (== Just MemoryTooLarge) . either Just (const Nothing)
    preInstantiate (codeFromWords [addi 0 0 0]) (B.pack [1, 2, 3, 4]) 8 4 `shouldSatisfy` (== Just MemoryTooLarge) . either Just (const Nothing)
    ok (B.replicate (fromIntegral kCodeCeilingBytes) 0x13 `seq` codeFromWords (replicate (fromIntegral (kCodeCeilingBytes `div` 4)) (addi 0 0 0)), B.empty, 0, 0) `shouldBe` True
    either Just (const Nothing) (preInstantiate (B.replicate (fromIntegral kCodeCeilingBytes + 4) 0) B.empty 0 0) `shouldBe` Just CodeTooLarge
  it "maps over-ceiling declarations under permissive limits to InvalidProgram" $ do
    let permissive = Rv32Limits maxBound maxBound
    createErr permissive (setU32At kArtifactMemoryBytesOffset (kMemoryCeilingBytes + 1) validArtifact) `shouldBe` Just ArtifactInvalidProgram
    createErr permissive (setU32At kArtifactMemoryBytesOffset kMemoryCeilingBytes validArtifact) `shouldBe` Nothing
  it "maps every artifact error to its wire reject code" $ do
    map toContractRejectCode [minBound .. maxBound]
      `shouldBe` [ ContractRejectMalformedArtifact
                 , ContractRejectMalformedArtifact
                 , ContractRejectUnsupportedFormat
                 , ContractRejectMalformedArtifact
                 , ContractRejectUnsupportedAbi
                 , ContractRejectUnsupportedAbi
                 , ContractRejectUnsupportedAbi
                 , ContractRejectUnsupportedAbi
                 , ContractRejectGasScheduleMismatch
                 , ContractRejectGasScheduleMismatch
                 , ContractRejectMemoryTooLarge
                 , ContractRejectCodeSizeInvalid
                 , ContractRejectInitialMemoryInvalid
                 , ContractRejectMalformedArtifact
                 , ContractRejectDigestMismatch
                 , ContractRejectDigestMismatch
                 , ContractRejectInvalidProgram
                 ]
  it "verifies blobs (Rv32Context::verify_contract)" $ do
    either Just (const Nothing) (verifyRv32Blob defaultLimits "garbage") `shouldBe` Just ContractRejectMalformedArtifact
    either Just (const Nothing) (verifyRv32Blob defaultLimits (B.take 200 Fx.kArtifact)) `shouldBe` Just ContractRejectMalformedArtifact
    either Just (const Nothing) (verifyRv32Blob defaultLimits (setU32At kArtifactGasVersionOffset 99 validArtifact)) `shouldBe` Just ContractRejectGasScheduleMismatch
    either Just (const Nothing) (verifyRv32Blob defaultLimits (buildArtifact [0x0020f1d3] B.empty 0)) `shouldBe` Just ContractRejectInvalidProgram
    verifyRv32Blob defaultLimits Fx.kArtifact `shouldSatisfy` isRight
  it "accepts runs exceeding the run-gas chunk cap (Rv32ContractTest.VerifyAcceptsRunExceedingMaxRunGas)" $ do
    let divs = replicate 5008 (op 3 1 2 4 1) ++ [kEcall]
    createErr defaultLimits (buildArtifact divs B.empty 0) `shouldBe` Nothing

------------------------------------------------------------------------------

decoderSpec :: Spec
decoderSpec = describe "decoder" $ do
  it "decodes representative instruction families" $ do
    let cases =
          [ (uType 0x37 5 0x12345000, KindLui, OpLui, Just 5, Nothing, Nothing, 0x12345000, kBaseInstructionGas)
          , (uType 0x17 6 0xfffff000, KindAuipc, OpAuipc, Just 6, Nothing, Nothing, -4096, kBaseInstructionGas)
          , (jType 7 (-2048), KindJal, OpJal, Just 7, Nothing, Nothing, -2048, kDirectJumpGas)
          , (jalr 8 9 12, KindJalr, OpJalr, Just 8, Just 9, Just 12, 12, kIndirectJumpGas)
          , (bType 0 1 2 (-16), KindBranch, OpBeq, Nothing, Just 1, Just 2, -16, kBranchInstructionGas)
          , (load 3 2 4 20, KindLoad, OpLw, Just 3, Just 4, Just 20, 20, kWideLoadGas)
          , (sType 2 5 6 (-12), KindStore, OpSw, Nothing, Just 5, Just 6, -12, kBaseInstructionGas)
          , (addi 10 11 (-1), KindOpImm, OpAddi, Just 10, Just 11, Just 31, -1, kBaseInstructionGas)
          , (rType 0 13 12 0 14, KindOp, OpAdd, Just 14, Just 12, Just 13, 0, kBaseInstructionGas)
          , (rType 1 13 12 4 14, KindOp, OpDiv, Just 14, Just 12, Just 13, 0, kDivInstructionGas)
          , (kFence, KindFence, OpFence, Just 0, Just 0, Just 0, 0, kBaseInstructionGas)
          , (kEcall, KindEcall, OpEcall, Just 0, Just 0, Just 0, 0, kBaseInstructionGas)
          , (kEbreak, KindEbreak, OpEbreak, Just 0, Just 0, Nothing, 0, kBaseInstructionGas)
          , (kUnimp, KindUnimp, OpUnimp, Just 0, Just 0, Nothing, 0, kBaseInstructionGas)
          ] ::
            [(Word32, InstructionKind, Op, Maybe Word8, Maybe Word8, Maybe Word8, Int32, Word64)]
    forM_ cases $ \(w, k, o, mrd, mrs1, mrs2, imm, g) -> case decodeInstruction w of
      Left e -> expectationFailure (show (w, e))
      Right d -> do
        (d.kind, d.op, d.immediate, d.gasCost, d.word) `shouldBe` (k, o, imm, g, w)
        forM_ mrd (d.rd `shouldBe`)
        forM_ mrs1 (d.rs1 `shouldBe`)
        forM_ mrs2 (d.rs2 `shouldBe`)
  it "validates code shape and profile" $ do
    validateProgram (B.singleton 0) `shouldBe` Left CodeSizeNotAligned
    validateProgram (B.replicate (fromIntegral kCodeCeilingBytes + 4) 0) `shouldBe` Left CodeTooLarge
    validateProgram (codeFromWords [0x00000001]) `shouldBe` Left UnsupportedCompressedInstruction
    validateProgram (codeFromWords [0x10500073]) `shouldBe` Left UnsupportedSystemInstruction
    validateProgram (codeFromWords [jType 0 8, kEbreak, kEcall]) `shouldBe` Right ()
    validateProgram (codeFromWords [0x0000100f]) `shouldBe` Left UnsupportedInstruction
    validateProgram (codeFromWords [0x0000007f]) `shouldBe` Left UnsupportedInstruction
    validateProgram (codeFromWords (replicate (fromIntegral (kDefaultMaxCodeBytes `div` 4)) (addi 0 0 0))) `shouldBe` Right ()
    validateProgram (codeFromWords [0x00001073]) `shouldBe` Left UnsupportedSystemInstruction
    validateProgram (codeFromWords [0x30200073]) `shouldBe` Left UnsupportedSystemInstruction
    validateProgram (codeFromWords [0x0000202f]) `shouldBe` Left UnsupportedInstruction
    validateProgram (codeFromWords [0x00000053]) `shouldBe` Left UnsupportedInstruction
    validateProgram (codeFromWords [0x00000007]) `shouldBe` Left UnsupportedInstruction
  it "creates an immutable canonical program" $ do
    case createProgram (codeFromWords [addi 1 0 7]) of
      Left e -> expectationFailure (show e)
      Right p -> do
        instructionCount p `shouldBe` 1
        programByteSize p `shouldBe` 4
        containsPc p 0 `shouldBe` True
        containsPc p 4 `shouldBe` False
        containsPc p 2 `shouldBe` False
        (instructionAtAlignedPc p 0).op `shouldBe` OpAddi
    either Just (const Nothing) (createProgram (B.singleton 0)) `shouldBe` Just CodeSizeNotAligned
  it "admits only the canonical UNIMP word among CSR forms" $ do
    (.op) <$> decodeInstruction 0xc0001073 `shouldBe` Right OpUnimp
    forM_ [0xc0002073, 0xc0101073, 0xc00010f3, 0xc0001173, 0xc0003073, 0x00000173, 0x10200073, 0x00200073] $ \w ->
      (w, either Just (const Nothing) (decodeInstruction w)) `shouldBe` (w, Just UnsupportedSystemInstruction)
  it "agrees with the independent spec decoder on every opcode/funct3/funct7 class" $ do
    let grid =
          [ (f7 * 0x2000000) + (f3 * 0x1000) + opc + filler
          | opc <- [0 .. 127]
          , f3 <- [0 .. 7]
          , f7 <- [0 .. 127]
          , let filler = ((opc * 7 + f3 * 13 + f7 * 29) .&. 0x1f) * 0x80 + ((opc + f7) .&. 0x1f) * 0x8000 + ((f3 * 5 + f7) .&. 0x1f) * 0x100000
          ]
    let mismatches = [w | w <- grid, not (decodersAgree w)]
    take 5 mismatches `shouldBe` []
  it "agrees with the independent spec decoder on arbitrary words" $
    property $ \w -> decodersAgree w
  it "agrees on every SYSTEM word that differs from ECALL/EBREAK/UNIMP in one bit" $ do
    let flips = [w `xor` (2 ^ b) | w <- [kEcall, kEbreak, kUnimp], b <- [0 .. 31 :: Int]]
    filter (not . decodersAgree) flips `shouldBe` []

-- | The library decoder and the mask/match spec decoder accept the same
-- words with the same error class, op, register fields, immediate and gas.
decodersAgree :: Word32 -> Bool
decodersAgree w = case (decodeInstruction w, specDecode w) of
  (Left e1, Left e2) -> e1 == e2
  (Right d, Right s) ->
    d.op == sOp s
      && fromIntegral d.rd == sRd s
      && fromIntegral d.rs1 == sRs1 s
      && fromIntegral d.rs2 == sRs2 s
      && d.immediate == sImm s
      && d.gasCost == specGas (sOp s)
  _ -> False

------------------------------------------------------------------------------

stepN :: Machine -> Int -> IO ()
stepN m n = forM_ [1 .. n] $ \_ -> step m `shouldReturn` StepContinue

executionSpec :: Spec
executionSpec = describe "execution" $ do
  describe "conformance cases (rv32_conformance_test)" $
    forM_ conformanceCases $ \c -> it c.ccName $ do
      m <- machineWithPc c.ccCode c.ccMemoryBytes c.ccGasLimit c.ccInitialPc
      forM_ c.ccInitialRegs (uncurry (setReg m))
      forM_ c.ccInitialMemory $ \(a, bs) -> writeGuest m a (B.pack bs) `shouldReturn` True
      r <- runWithHost haltWithA0Host m
      r `shouldBe` c.ccExpectedResult
      trap m `shouldReturn` c.ccExpectedTrap
      halted m `shouldReturn` True
      when (c.ccExpectedResult == StepHalted) $ haltCode m `shouldReturn` c.ccExpectedHaltCode
      forM_ c.ccExpectedPc (pc m `shouldReturn`)
      forM_ c.ccExpectedGasConsumed $ \g -> ((c.ccGasLimit -) <$> gasRemaining m) `shouldReturn` g
      forM_ c.ccExpectedRegs $ \(i, v) -> (i,) <$> reg m i `shouldReturn` (i, v)
      forM_ c.ccExpectedMemory $ \(a, bs) -> readGuest m a (fromIntegral (length bs)) `shouldReturn` Just (B.pack bs)
  it "keeps x0 hardwired and seeds sp to zero" $ do
    m <- machineFor [addi 0 0 123, addi 1 0 7] 31 10
    reg m 2 `shouldReturn` 0
    step m `shouldReturn` StepContinue
    reg m 0 `shouldReturn` 0
    step m `shouldReturn` StepContinue
    reg m 1 `shouldReturn` 7
  it "executes arithmetic, logic and shift operations" $ do
    m <- machineFor [rType 0 2 1 0 3, rType 0x20 2 1 0 4, rType 0 2 1 1 5, rType 0 2 1 2 6, rType 0 2 1 3 7, rType 0 2 1 4 8, rType 0 2 1 5 9, rType 0x20 2 1 5 10, rType 0 2 1 6 11, rType 0 2 1 7 12, iType 0x13 13 1 1 31] 1024 100
    setReg m 1 0x80000001
    setReg m 2 33
    stepN m 11
    mapM (reg m) [3 .. 13]
      `shouldReturn` [0x80000022, 0x7fffffe0, 0x00000002, 1, 0, 0x80000020, 0x40000000, 0xc0000000, 0x80000021, 0x00000001, 0x80000000]
  it "masks register shift amounts" $ do
    m <- machineFor [rType 0 2 1 1 3, rType 0 2 1 5 4, rType 0x20 2 1 5 5] 1024 10
    setReg m 1 0x80000001
    setReg m 2 40
    stepN m 3
    mapM (reg m) [3, 4, 5] `shouldReturn` [0x00000100, 0x00800000, 0xff800000]
  it "executes lui and auipc" $ do
    m <- machineFor [uType 0x37 1 0x12345000, uType 0x17 2 0x00001000] 1024 10
    stepN m 1
    reg m 1 `shouldReturn` 0x12345000
    stepN m 1
    reg m 2 `shouldReturn` 0x00001004
  it "executes immediate operations" $ do
    m <- machineFor [iType 0x13 3 2 1 0, iType 0x13 4 3 1 1, iType 0x13 5 4 1 0xff, iType 0x13 6 6 1 0xf0, iType 0x13 7 7 1 0xf0, iType 0x13 8 5 2 1, iType 0x13 9 5 2 0x401] 1024 100
    setReg m 1 0xffffffff
    setReg m 2 0x80000000
    stepN m 7
    mapM (reg m) [3 .. 9] `shouldReturn` [1, 0, 0xffffff00, 0xffffffff, 0xf0, 0x40000000, 0xc0000000]
    gasRemaining m `shouldReturn` 93
  it "executes byte/halfword stores and fence" $ do
    m1 <- machineFor [sType 0 1 2 0] 16 10
    setReg m1 1 4
    setReg m1 2 0xaabbccdd
    stepN m1 1
    readGuest m1 4 2 `shouldReturn` Just (B.pack [0xdd, 0])
    pc m1 `shouldReturn` 4
    gasRemaining m1 `shouldReturn` 9
    m2 <- machineFor [sType 1 1 2 0] 16 10
    setReg m2 1 6
    setReg m2 2 0xaabbccdd
    stepN m2 1
    readGuest m2 6 3 `shouldReturn` Just (B.pack [0xdd, 0xcc, 0])
    m3 <- machineFor [kFence] 16 10
    stepN m3 1
    pc m3 `shouldReturn` 4
    gasRemaining m3 `shouldReturn` 9
  it "traps ebreak and unimp deterministically after charging" $ do
    forM_ [(kEbreak, TrapBreakpoint), (kUnimp, TrapInvalidInstruction)] $ \(w, t) -> do
      m <- machineFor [w] 16 10
      step m `shouldReturn` StepTrap
      trap m `shouldReturn` t
      pc m `shouldReturn` 0
      gasRemaining m `shouldReturn` 9
      oog <- machineFor [w] 16 0
      step oog `shouldReturn` StepTrap
      trap oog `shouldReturn` TrapOutOfGas
      gasRemaining oog `shouldReturn` 0
  it "sign- and zero-extends loads" $ do
    m <- machineFor [addi 1 0 16, addi 2 0 (-1), sType 2 1 2 0, load 3 0 1 0, load 4 4 1 0, load 5 1 1 0, load 6 5 1 0, load 7 2 1 0] 64 100
    stepN m 8
    mapM (reg m) [3 .. 7] `shouldReturn` [0xffffffff, 0xff, 0xffffffff, 0xffff, 0xffffffff]
  it "executes branches and jumps" $ do
    m <- machineFor [addi 1 0 1, bType 0 1 1 8, addi 2 0 1, addi 2 0 2, jType 3 8, addi 4 0 1, addi 4 0 4] 1024 100
    stepN m 5
    mapM (reg m) [2, 3, 4] `shouldReturn` [2, 20, 4]
  it "executes all branch conditions, signed and unsigned" $
    forM_
      [ (0, 5, 5, 2), (0, 5, 6, 1), (1, 5, 6, 2), (1, 5, 5, 1), (4, 0xffffffff, 1, 2), (4, 1, 0xffffffff, 1), (4, 5, 5, 1)
      , (5, 1, 0xffffffff, 2), (5, 0xffffffff, 1, 1), (5, 5, 5, 2), (6, 1, 0xffffffff, 2), (6, 0xffffffff, 1, 1), (6, 5, 5, 1)
      , (7, 0xffffffff, 1, 2), (7, 1, 0xffffffff, 1), (7, 5, 5, 2)
      ]
      $ \(f3, l, r, e) -> do
        m <- machineFor [bType f3 1 2 8, addi 3 0 1, addi 3 0 2] 1024 10
        setReg m 1 l
        setReg m 2 r
        stepN m 2
        (f3, l, r) `seq` reg m 3 `shouldReturn` e
  it "traps bad jump targets before state mutation" $ do
    m1 <- machineFor [jType 1 2, addi 2 0 1] 1024 100
    step m1 `shouldReturn` StepTrap
    trap m1 `shouldReturn` TrapBadPc
    pc m1 `shouldReturn` 0
    reg m1 1 `shouldReturn` 0
    gasRemaining m1 `shouldReturn` (100 - kDirectJumpGas)
    m2 <- machineFor [jalr 1 2 0, addi 3 0 1] 1024 100
    setReg m2 2 3
    step m2 `shouldReturn` StepTrap
    trap m2 `shouldReturn` TrapBadPc
    reg m2 1 `shouldReturn` 0
    m3 <- machineFor [addi 1 0 1, bType 0 1 1 2, addi 2 0 1] 1024 100
    stepN m3 1
    step m3 `shouldReturn` StepTrap
    trap m3 `shouldReturn` TrapBadPc
    pc m3 `shouldReturn` 4
  it "clears the low jalr target bit and wraps jalr arithmetic" $ do
    m <- machineFor [jalr 2 1 0, addi 3 0 1, addi 3 0 2] 1024 100
    setReg m 1 9
    stepN m 1
    pc m `shouldReturn` 8
    reg m 2 `shouldReturn` 4
    stepN m 1
    reg m 3 `shouldReturn` 2
    w1 <- machineFor [jalr 5 1 1] 16 100
    setReg w1 1 maxBound
    stepN w1 1
    pc w1 `shouldReturn` 0
    reg w1 5 `shouldReturn` 4
    gasRemaining w1 `shouldReturn` (100 - kIndirectJumpGas)
    w2 <- machineFor [jalr 5 1 (-1)] 16 100
    step w2 `shouldReturn` StepTrap
    trap w2 `shouldReturn` TrapBadPc
    reg w2 5 `shouldReturn` 0
    gasRemaining w2 `shouldReturn` (100 - kIndirectJumpGas)
  it "applies fetch and data-memory access semantics" $ do
    forM_ [2, 4] $ \p -> do
      m <- machineWithPc [addi 1 0 1] 1024 10 p
      step m `shouldReturn` StepTrap
      trap m `shouldReturn` TrapBadPc
      gasRemaining m `shouldReturn` 10
    let one ws r v = do
          m <- machineFor ws 16 10
          setReg m r v
          step m
    one [load 1 2 2 0] 2 1 `shouldReturn` StepContinue -- unaligned, in bounds
    one [load 1 2 2 0] 2 16 `shouldReturn` StepTrap
    one [sType 2 1 2 0] 1 2 `shouldReturn` StepContinue
    one [sType 2 1 2 0] 1 16 `shouldReturn` StepTrap
  it "traps a zero-seeded stack underflow without mutating memory" $ do
    m <- machineFor [addi 1 0 0x7f, addi 2 2 (-4), sType 2 2 1 0] 16 10
    stepN m 2
    reg m 2 `shouldReturn` 0xfffffffc
    step m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapBadMemory
    gasRemaining m `shouldReturn` 7
    memorySnapshot m `shouldReturn` B.replicate 16 0
  it "faults every access of a zero-sized memory" $
    forM_ [load 1 2 0 0, load 1 0 0 0, sType 0 0 0 0] $ \w -> do
      m <- machineFor [w] 0 100
      step m `shouldReturn` StepTrap
      trap m `shouldReturn` TrapBadMemory
  it "debits gas on load/store traps without state mutation" $ do
    forM_ [(0, 16, kBaseInstructionGas), (1, 16, kWideLoadGas), (2, 16, kWideLoadGas), (1, 15, kWideLoadGas), (2, 13, kWideLoadGas)] $ \(f3, a, g) -> do
      m <- machineFor [load 1 f3 2 0] 16 g
      setReg m 1 99
      setReg m 2 a
      step m `shouldReturn` StepTrap
      trap m `shouldReturn` TrapBadMemory
      gasRemaining m `shouldReturn` 0
      pc m `shouldReturn` 0
      reg m 1 `shouldReturn` 99
    forM_ [(0, 16), (1, 16), (2, 16), (1, 15), (2, 13)] $ \(f3, a) -> do
      m <- machineFor [sType f3 1 2 0] 16 kBaseInstructionGas
      setReg m 1 a
      setReg m 2 0xaabbccdd
      step m `shouldReturn` StepTrap
      trap m `shouldReturn` TrapBadMemory
      gasRemaining m `shouldReturn` 0
      memorySnapshot m `shouldReturn` B.replicate 16 0
  it "traps running off the end as a bad PC" $ do
    m <- machineFor [addi 1 0 1] 1024 10
    stepN m 1
    pc m `shouldReturn` 4
    step m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapBadPc
    gasRemaining m `shouldReturn` 9
  it "implements M-extension edge cases" $ do
    m <- machineFor [rType 1 2 1 f3 (3 + f3) | f3 <- [0 .. 7]] 1024 100
    setReg m 1 0x80000000
    setReg m 2 0xffffffff
    stepN m 8
    mapM (reg m) [3 .. 10] `shouldReturn` [0x80000000, 0, 0x80000000, 0x7fffffff, 0x80000000, 0, 0, 0x80000000]
    z <- machineFor [rType 1 2 1 f3 (f3 - 1) | f3 <- [4 .. 7]] 1024 100
    setReg z 1 123
    setReg z 2 0
    stepN z 4
    mapM (reg z) [3 .. 6] `shouldReturn` [0xffffffff, 0xffffffff, 123, 123]
  it "charges mul/div costs before mutation" $
    forM_ [(0, 1, 252), (1, 1, 0), (2, 1, 0), (3, 1, 0), (4, 2, 7), (5, 2, 7), (6, 2, 0), (7, 2, 0)] $ \(f3, cost, e) -> do
      m <- machineFor [rType 1 2 1 f3 3] 1024 (cost - 1)
      setReg m 1 42
      setReg m 2 6
      step m `shouldReturn` StepTrap
      trap m `shouldReturn` TrapOutOfGas
      reg m 3 `shouldReturn` 0
      gasRemaining m `shouldReturn` 0
      m' <- machineFor [rType 1 2 1 f3 3] 1024 cost
      setReg m' 1 42
      setReg m' 2 6
      step m' `shouldReturn` StepContinue
      reg m' 3 `shouldReturn` e
      gasRemaining m' `shouldReturn` 0
  it "runs a representative loop program" $ do
    m <- machineFor [addi 1 0 5, addi 2 0 0, rType 0 1 2 0 2, addi 1 1 (-1), bType 1 1 0 (-8), kEcall] 1024 100
    runWithHost (haltHost 77) m `shouldReturn` StepHalted
    reg m 2 `shouldReturn` 15
    haltCode m `shouldReturn` 77
  it "bounds-checks stores deterministically over random addresses" $
    property $
      forAll (choose (0, 40)) $ \addr -> ioProperty $ do
        m <- machineFor [sType 2 1 2 0] 32 10
        setReg m 1 addr
        setReg m 2 0xaabbccdd
        r <- step m
        if addr + 4 > 32
          then (\t -> r == StepTrap && t == TrapBadMemory) <$> trap m
          else (\bs -> r == StepContinue && bs == Just (B.pack [0xdd, 0xcc, 0xbb, 0xaa])) <$> readGuest m addr 4
  it "checks host-view memory ranges (Rv32HostViewTest)" $ do
    memoryRangeInBounds 16 12 4 `shouldBe` True
    memoryRangeInBounds 16 16 0 `shouldBe` True
    memoryRangeInBounds 16 17 0 `shouldBe` False
    memoryRangeInBounds 16 1 maxBound `shouldBe` False
    memoryRangeInBounds 16 maxBound maxBound `shouldBe` False
    m <- machineFor [addi 0 0 0] 16 10
    writeGuest m 12 (B.pack [1, 2, 3, 4]) `shouldReturn` True
    readGuest m 12 4 `shouldReturn` Just (B.pack [1, 2, 3, 4])
    before <- memorySnapshot m
    writeGuest m 16 B.empty `shouldReturn` True
    readGuest m 16 0 `shouldReturn` Just B.empty
    writeGuest m 16 (B.singleton 9) `shouldReturn` False
    writeGuest m 13 (B.pack [1, 2, 3, 4]) `shouldReturn` False
    readGuest m 13 4 `shouldReturn` Nothing
    writeGuest m maxBound (B.singleton 9) `shouldReturn` False
    readGuest m maxBound 4 `shouldReturn` Nothing
    readGuest m 0 maxBound `shouldReturn` Nothing
    memorySnapshot m `shouldReturn` before
  it "seeds the method id into a0/a1 and leaves other registers zero" $ do
    case prepareWords [addi 0 0 0] 4096 of
      Left e -> expectationFailure (show e)
      Right p -> do
        m <- instantiate p 0xCA571C66A719DD56 10
        reg m 10 `shouldReturn` 0xA719DD56
        reg m 11 `shouldReturn` 0xCA571C66
        rs <- regs m
        [r | (i, r) <- zip [0 :: Int ..] rs, i /= 10, i /= 11] `shouldSatisfy` all (== 0)
        pc m `shouldReturn` 0
  it "makes the seeded method id live at execution start" $
    case prepareWords [addi 5 10 0, addi 6 11 0, kEbreak] 16 of
      Left e -> expectationFailure (show e)
      Right p -> do
        m <- instantiate p 0x0000000700000003 100
        start m `shouldReturn` StepTrap
        mapM (reg m) [5, 6] `shouldReturn` [3, 7]
  it "keeps instantiations of one prepared contract independent" $
    case preInstantiate (codeFromWords [addi 1 0 1, sType 2 0 1 0, kEbreak]) (B.pack [9, 9, 9, 9]) 0 8 of
      Left e -> expectationFailure (show e)
      Right p -> do
        m1 <- newMachine p 100
        m2 <- newMachine p 100
        start m1 `shouldReturn` StepTrap
        memorySnapshot m1 `shouldReturn` B.pack [1, 0, 0, 0, 0, 0, 0, 0]
        memorySnapshot m2 `shouldReturn` B.pack [9, 9, 9, 9, 0, 0, 0, 0]
        reg m2 1 `shouldReturn` 0
  it "runs millions of instructions" $ do
    -- x1 = 1_000_000; loop { x2 += x1; x1 -= 1 } while x1 != 0: 3M instructions.
    m <- machineFor [uType 0x37 1 0xf4000, addi 1 1 0x240, rType 0 1 2 0 2, addi 1 1 (-1), bType 1 1 0 (-8), kEbreak] 16 100000000
    start m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapBreakpoint
    reg m 2 `shouldReturn` fromIntegral ((1000000 * 1000001 `div` 2) `mod` (2 ^ (32 :: Int)) :: Integer)
    gasRemaining m `shouldReturn` (100000000 - (2 + 1000000 * 4 + 1))

------------------------------------------------------------------------------

borrowedSpec :: Spec
borrowedSpec = describe "riscv-tests vectors" $ do
  let expectHalts m = do
        runWithHost (haltHost 0) m `shouldReturn` StepHalted
        trap m `shouldReturn` TrapNone
  describe "register-register" $
    forM_ rrVectors $ \(RrVector name f7 f3 lhs rhs expected) -> it name $
      forM_ [3, 1, 2, 0] $ \rd -> do
        m <- machineFor [rType f7 2 1 f3 rd, kEcall] 256 1000
        setReg m 1 lhs
        setReg m 2 rhs
        expectHalts m
        (rd,) <$> reg m (fromIntegral rd) `shouldReturn` (rd, if rd == 0 then 0 else expected)
  describe "register-immediate" $
    forM_ immVectors $ \(ImmVector name f3 imm lhs expected) -> it name $
      forM_ [3, 1, 0] $ \rd -> do
        m <- machineFor [iType 0x13 rd f3 1 imm, kEcall] 256 1000
        setReg m 1 lhs
        expectHalts m
        (rd,) <$> reg m (fromIntegral rd) `shouldReturn` (rd, if rd == 0 then 0 else expected)
  describe "branches" $
    forM_ branchVectors $ \(BranchVector name f3 lhs rhs taken) -> it name $ do
      fwd <- machineFor [bType f3 1 2 12, addi 3 0 1, kEcall, addi 3 0 2, kEcall] 256 1000
      setReg fwd 1 lhs
      setReg fwd 2 rhs
      expectHalts fwd
      reg fwd 3 `shouldReturn` (if taken then 2 else 1)
      back <- machineWithPc [addi 3 0 2, kEcall, bType f3 1 2 (-8), addi 3 0 1, kEcall] 256 1000 8
      setReg back 1 lhs
      setReg back 2 rhs
      expectHalts back
      reg back 3 `shouldReturn` (if taken then 2 else 1)
  describe "loads" $
    forM_ loadVectors $ \(LoadVector name f3 bytes addr offset expected) -> it name $
      forM_ [3, 1, 0] $ \rd -> do
        m <- machineFor [load rd f3 1 offset, kEcall] 256 1000
        writeGuest m kDataAddress (B.pack bytes) `shouldReturn` True
        setReg m 1 (addr - fromIntegral offset)
        expectHalts m
        (rd,) <$> reg m (fromIntegral rd) `shouldReturn` (rd, if rd == 0 then 0 else expected)
  describe "stores" $
    forM_ storeVectors $ \(StoreVector name f3 value addr offset expected) -> it name $ do
      m <- machineFor [sType f3 1 2 offset, kEcall] 256 1000
      writeGuest m (kDataAddress - 8) (B.replicate 128 0xee) `shouldReturn` True
      setReg m 1 (addr - fromIntegral offset)
      setReg m 2 value
      expectHalts m
      readGuest m addr (fromIntegral (length expected)) `shouldReturn` Just (B.pack expected)
      region <- maybe B.empty id <$> readGuest m (kDataAddress - 8) 128
      let clobberEnd = addr + fromIntegral (length expected)
      [ (a, b) | (a, b) <- zip [kDataAddress - 8 ..] (B.unpack region), a < addr || a >= clobberEnd, b /= 0xee] `shouldBe` []
  describe "upper immediates" $ do
    forM_ upperVectors $ \(UpperVector name opc imm initialPc expected) -> it name $ do
      let ws = replicate (fromIntegral (initialPc `div` 4)) (addi 0 0 0) ++ [uType opc 3 imm, kEcall]
      m <- machineWithPc ws 256 1000 initialPc
      expectHalts m
      reg m 3 `shouldReturn` expected
    it "lui to x0 keeps zero" $ do
      m <- machineFor [uType 0x37 0 0x80000000, kEcall] 256 1000
      expectHalts m
      reg m 0 `shouldReturn` 0
  describe "jumps" $
    forM_ jumpVectors $ \(JumpVector name first x1 linkReg expectedLink marker) -> it name $ do
      m <- machineFor [first, addi 5 0 0x11, kEcall, addi 5 0 0x33, kEcall] 256 1000
      setReg m 1 x1
      expectHalts m
      reg m linkReg `shouldReturn` expectedLink
      reg m 5 `shouldReturn` marker

------------------------------------------------------------------------------

yieldSpec :: Spec
yieldSpec = describe "yield API" $ do
  it "start yields at an ecall, resume continues" $ do
    m <- machineFor [addi 10 0 1, kEcall, addi 11 0 5] 64 100
    start m `shouldReturn` StepEcall
    ((.a0) <$>) <$> pendingEcall m `shouldReturn` Just 1
    pc m `shouldReturn` 8
    resumeFromEcall m (continueWith 9 0) `shouldReturn` StepTrap
    trap m `shouldReturn` TrapBadPc
    reg m 10 `shouldReturn` 9
    reg m 11 `shouldReturn` 5
  it "exposes a0..a7 at the ecall and the post-ecall gas" $ do
    m <- machineFor (map (\r -> addi r 0 (fromIntegral r * 3)) [10 .. 17] ++ [kEcall]) 16 100
    start m `shouldReturn` StepEcall
    pendingEcall m `shouldReturn` Just (EcallArgs 30 33 36 39 42 45 48 51)
    gasRemaining m `shouldReturn` 91
    -- start with the ecall still pending re-yields it.
    start m `shouldReturn` StepEcall
    step m `shouldReturn` StepEcall
  it "halt is terminal; repeated start keeps reporting it" $ do
    m <- machineFor [kEcall] 16 100
    start m `shouldReturn` StepEcall
    resumeFromEcall m (HostHalt 7 0) `shouldReturn` StepHalted
    haltCode m `shouldReturn` 7
    halted m `shouldReturn` True
    trap m `shouldReturn` TrapNone
    start m `shouldReturn` StepHalted
    pendingEcall m `shouldReturn` Nothing
  it "a trapped machine keeps reporting the trap" $ do
    m <- machineFor [kEbreak] 16 100
    start m `shouldReturn` StepTrap
    start m `shouldReturn` StepTrap
    step m `shouldReturn` StepTrap
    gasRemaining m `shouldReturn` 99
  it "host memory writes persist on continue and on host out-of-gas" $ do
    forM_ [continueWith 0 0, HostOutOfGas 0] $ \hr -> do
      m <- machineFor [kEcall] 16 100
      start m `shouldReturn` StepEcall
      writeGuest m 4 (B.pack [0xaa, 0xbb, 0xcc, 0xdd]) `shouldReturn` True
      _ <- resumeFromEcall m hr
      readGuest m 4 4 `shouldReturn` Just (B.pack [0xaa, 0xbb, 0xcc, 0xdd])
  it "host out-of-gas keeps pc advanced, zeroes gas and leaves a0" $ do
    m <- machineFor [kEcall] 1024 100
    setReg m 10 7
    start m `shouldReturn` StepEcall
    resumeFromEcall m (HostOutOfGas 0) `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
    reg m 10 `shouldReturn` 7
    pc m `shouldReturn` 4
    gasRemaining m `shouldReturn` 0
    pendingEcall m `shouldReturn` Nothing
  it "an unaffordable ecall never reaches the host" $ do
    m <- machineFor [kEcall] 16 0
    setReg m 10 7
    start m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
    pc m `shouldReturn` 0
    reg m 10 `shouldReturn` 7
    pendingEcall m `shouldReturn` Nothing
  it "continue then halt (ContinueThenHaltHost)" $ do
    m <- machineFor [kEcall, addi 1 10 1, kEcall] 1024 100
    calls <- newIORef (0 :: Int)
    let host _ _ = do
          modifyIORef' calls (+ 1)
          n <- readIORef calls
          pure (if n == 1 then continueWith 123 0 else HostHalt 0 0)
    runWithHost host m `shouldReturn` StepHalted
    readIORef calls `shouldReturn` 2
    reg m 1 `shouldReturn` 124
  it "reset restores all mutable execution state" $
    case preInstantiate (codeFromWords [addi 1 0 7, kEcall, addi 3 0 9]) (B.pack [1, 2, 3, 4]) 4 16 of
      Left e -> expectationFailure (show e)
      Right p -> do
        m <- newMachine p 100
        _ <- writeGuest m 0 (B.singleton 0xaa)
        start m `shouldReturn` StepEcall
        reg m 1 `shouldReturn` 7
        reset m 55
        pc m `shouldReturn` 0
        reg m 1 `shouldReturn` 0
        gasRemaining m `shouldReturn` 55
        trap m `shouldReturn` TrapNone
        halted m `shouldReturn` False
        pendingEcall m `shouldReturn` Nothing
        readGuest m 0 8 `shouldReturn` Just (B.pack [0, 0, 0, 0, 1, 2, 3, 4])
        start m `shouldReturn` StepEcall
        resumeFromEcall m (continueWith 0 0) `shouldReturn` StepTrap
        trap m `shouldReturn` TrapBadPc
        reset m 44
        trap m `shouldReturn` TrapNone
        halted m `shouldReturn` False
        start m `shouldReturn` StepEcall
        resumeFromEcall m (HostHalt 9 0) `shouldReturn` StepHalted
        reset m 33
        halted m `shouldReturn` False
        haltCode m `shouldReturn` 0
        gasRemaining m `shouldReturn` 33
        start m `shouldReturn` StepEcall
  it "drives the same final state via start/resume as via a host loop" $ do
    let host mm _ = writeGuest mm 8 (B.pack [1, 2, 3, 4]) >> pure (HostHalt 0 0)
    m <- machineFor [addi 10 0 1, kEcall] 64 100
    runWithHost host m `shouldReturn` StepHalted
    pc m `shouldReturn` 8
    readGuest m 8 4 `shouldReturn` Just (B.pack [1, 2, 3, 4])

------------------------------------------------------------------------------

-- | The canonical expectations of rv32_threaded_test's trap-gas parity
-- cases (production must and does match these; see the Machine header).
expectTrapGas :: [Word32] -> Trap -> Word64 -> Expectation
expectTrapGas ws t g = do
  m <- machineFor ws 256 100
  runWithHost (continueHost 0 0) m `shouldReturn` StepTrap
  trap m `shouldReturn` t
  gasRemaining m `shouldReturn` g

gasSpec :: Spec
gasSpec = describe "gas and traps" $ do
  it "traps misaligned control-flow targets after charging" $ do
    expectTrapGas [jType 0 6, addi 1 0 1] TrapBadPc (100 - kDirectJumpGas)
    expectTrapGas [bType 0 0 0 6, addi 1 0 1] TrapBadPc (100 - kBranchInstructionGas)
    expectTrapGas [addi 1 0 7, jalr 0 1 0, addi 3 0 9] TrapBadPc (100 - kBaseInstructionGas - kIndirectJumpGas)
  it "treats the exact code end as an invalid jump target" $ do
    expectTrapGas [addi 1 0 1, jType 0 4] TrapBadPc (100 - kBaseInstructionGas - kDirectJumpGas)
    expectTrapGas [addi 1 0 8, jalr 0 1 0] TrapBadPc (100 - kBaseInstructionGas - kIndirectJumpGas)
  it "traps empty code as a bad PC without charging" $
    case preInstantiate B.empty B.empty 0 0 of
      Left e -> expectationFailure (show e)
      Right p -> do
        m <- newMachine p 100
        start m `shouldReturn` StepTrap
        trap m `shouldReturn` TrapBadPc
        gasRemaining m `shouldReturn` 100
  it "charges exactly the executed prefix on mid-run memory faults" $ do
    expectTrapGas [addi 1 0 1024, addi 6 0 5, load 4 2 1 0, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory (100 - 2 - kWideLoadGas)
    expectTrapGas [addi 1 0 1024, addi 2 0 7, store 1 2 0 2, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory 97
    expectTrapGas [load 4 2 0 2047, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory (100 - kWideLoadGas)
    expectTrapGas [op 6 0 0 0 0, load 4 2 0 2047, kEcall] TrapBadMemory (100 - 1 - kWideLoadGas)
    expectTrapGas [op 6 0 0 0 0, load 4 2 0 2047] TrapBadMemory (100 - 1 - kWideLoadGas)
    expectTrapGas [branch 0 0 0 8, addi 6 0 1, load 4 2 0 2047, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory (100 - kBranchInstructionGas - kWideLoadGas)
  it "charges exactly the executed prefix on faults inside production-fused pairs" $ do
    -- lw;add (load-use), lw;addi, addi;lw, addi;sw, sw;addi, lw;sw head and tail.
    expectTrapGas [addi 1 0 1024, addi 6 0 5, load 4 2 1 0, op 5 4 6 0 0, op 7 6 6 0 0] TrapBadMemory (100 - 2 - kWideLoadGas)
    expectTrapGas [addi 1 0 1024, addi 6 0 5, load 4 2 1 0, addi 5 4 1, op 7 6 6 0 0] TrapBadMemory (100 - 2 - kWideLoadGas)
    expectTrapGas [op 6 0 0 0 0, addi 1 0 1024, load 4 2 1 0, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory (100 - 2 - kWideLoadGas)
    expectTrapGas [op 6 0 0 0 0, addi 1 0 1024, store 1 1 0 2, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory 97
    expectTrapGas [uType 0x37 1 4096, store 1 1 0 2, addi 7 7 4, op 8 6 6 0 0] TrapBadMemory 98
    expectTrapGas [load 4 2 0 2047, store 0 4 0 2, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory (100 - kWideLoadGas)
    expectTrapGas [load 4 2 0 4, store 0 4 2044 2, op 7 6 6 0 0, op 8 6 6 0 0] TrapBadMemory (100 - kWideLoadGas - 1)
    expectTrapGas [addi 1 0 254, addi 2 0 7, store 1 2 0 2, op 7 6 6 0 0] TrapBadMemory 97
  it "sweeps budgets across a mid-run faulting program" $ do
    let ws = [addi 1 0 1024, addi 6 0 5, load 4 2 1 0, op 7 6 6 0 0, op 8 6 6 0 0]
        prefix = 2 + kWideLoadGas
    forM_ [0 .. prefix + 2 + 3] $ \g -> do
      m <- machineFor ws 256 g
      start m `shouldReturn` StepTrap
      (g,) <$> trap m `shouldReturn` (g, if g >= prefix then TrapBadMemory else TrapOutOfGas)
      (g,) <$> gasRemaining m `shouldReturn` (g, if g >= prefix then g - prefix else 0)
  it "meters a tight self loop until out of gas" $ do
    m <- machineFor [branch 0 0 0 0] 256 8
    start m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
    gasRemaining m `shouldReturn` 0
  it "reports out of gas (not the run's trap) when the run is unaffordable" $ do
    -- Production runs these in deficit; the classification is out of gas.
    let oog ws g = do
          m <- machineFor ws 256 g
          start m `shouldReturn` StepTrap
          trap m `shouldReturn` TrapOutOfGas
          gasRemaining m `shouldReturn` 0
    oog [addi 1 0 1, kEbreak] 1
    oog [addi 1 0 1, kUnimp] 1
    oog [addi 1 0 1, branch 0 0 0 4096] 2
    oog [addi 1 0 1, jType 0 4096] 3
    oog [addi 1 0 1, addi 2 0 2] 1
    oog [kFence, uType 0x37 1 4096, load 2 2 1 0] 2
    oog [addi 1 0 1, kEcall] 1
  it "keeps the canonical value at an ecall reached with the exact budget" $ do
    m <- machineFor [addi 1 0 1, kEcall] 256 2
    start m `shouldReturn` StepEcall
    gasRemaining m `shouldReturn` 0
    resumeFromEcall m (continueWith 5 0) `shouldReturn` StepTrap
    trap m `shouldReturn` TrapBadPc
    reg m 10 `shouldReturn` 5
  it "charges host gas identically on continue and traps when it is exhausted" $ do
    m <- machineFor [kEcall, addi 1 10 0, kEcall] 256 20
    start m `shouldReturn` StepEcall
    resumeFromEcall m (continueWith 3 10) `shouldReturn` StepEcall
    gasRemaining m `shouldReturn` (20 - 1 - 10 - 1 - 1)
    reg m 1 `shouldReturn` 3
    resumeFromEcall m (continueWith 4 8) `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
    gasRemaining m `shouldReturn` 0
    reg m 10 `shouldReturn` 3 -- a0 not written by the unaffordable result
    pc m `shouldReturn` 12
  it "stops before an unfunded host call (repeatedEcallsStopBeforeAnUnfundedHostCall)" $ do
    let limit = 2 * (kBaseInstructionGas + 2 + kDirectJumpGas)
    m <- machineFor [kEcall, jType 0 (-4)] 16 limit
    calls <- newIORef (0 :: Word32)
    let host _ _ = modifyIORef' calls (+ 1) >> (\n -> continueWith n 2) <$> readIORef calls
    runWithHost host m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
    gasRemaining m `shouldReturn` 0
    readIORef calls `shouldReturn` 2
    reg m 10 `shouldReturn` 2
  it "consumeGas debits, and zeroes without trapping when over budget" $ do
    m <- machineFor [kEcall] 16 10
    start m `shouldReturn` StepEcall
    consumeGas m 4 `shouldReturn` True
    gasRemaining m `shouldReturn` 5
    consumeGas m 5 `shouldReturn` True
    gasRemaining m `shouldReturn` 0
    consumeGas m 0 `shouldReturn` True
    m2 <- machineFor [kEcall] 16 10
    start m2 `shouldReturn` StepEcall
    consumeGas m2 10 `shouldReturn` False
    gasRemaining m2 `shouldReturn` 0
    trap m2 `shouldReturn` TrapNone
  it "matches canonical metering across split runs (splitRunBudgetSweepMatchesCanonical)" $ do
    let n = 8200 :: Word64
        ws = replicate (fromIntegral n) (op 3 1 2 4 1) ++ [kEcall]
        total = n * kDivInstructionGas + 1
    forM_ [0, 1, 8, 9999, 10000, 10001, 59999, 60000, 60001, total - 1, total, total + 1, 1000000000] $ \g -> do
      m <- machineFor ws 256 g
      r <- start m
      if g >= total
        then do
          (g, r) `shouldBe` (g, StepEcall)
          gasRemaining m `shouldReturn` (g - total)
        else do
          (g, r) `shouldBe` (g, StepTrap)
          trap m `shouldReturn` TrapOutOfGas
          gasRemaining m `shouldReturn` 0
  it "runs normally with a huge gas budget" $ do
    m <- machineFor [addi 1 0 1, kEcall] 16 maxBound
    start m `shouldReturn` StepEcall
    gasRemaining m `shouldReturn` (maxBound - 2)
    resumeFromEcall m (continueWith 0 maxBound) `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
  it "traps wrapped addresses like any out-of-bounds access" $ do
    forM_ [load 2 2 1 0, load 2 0 1 0, sType 2 1 0 0, load 2 1 1 (-1)] $ \w -> do
      m <- machineFor [w] 256 100
      setReg m 1 0xffffffff
      start m `shouldReturn` StepTrap
      trap m `shouldReturn` TrapBadMemory
    -- base + imm wraps modulo 2^32: 0xffffffff + 1 is address 0, in bounds.
    m <- machineFor [load 2 1 1 1, kEbreak] 256 100
    _ <- writeGuest m 0 (B.pack [0x34, 0x12])
    setReg m 1 0xffffffff
    start m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapBreakpoint
    reg m 2 `shouldReturn` 0x1234

------------------------------------------------------------------------------

fixtureSpec :: Spec
fixtureSpec = describe "compiled sha256/return fixture" $ do
  let prepared = either (error . show) id (createContract defaultLimits Fx.kArtifact)
      -- syscall 0: SHA256(a1 = ptr, a2 = len) -> 32 bytes at a3; 6: Return.
      shaCost len = sha256Hash (fromIntegral len) + kSyscallBaseCost
      runMethod methodId gas = do
        m <- instantiateContract prepared methodId gas
        r1 <- start m
        r1 `shouldBe` StepEcall
        Just sha <- pendingEcall m
        shaGas <- gasRemaining m
        sha.a0 `shouldBe` 0
        (sha.a1, sha.a2) `shouldBe` (Fx.kFixtureInputOffset, 7)
        Just input <- readGuest m sha.a1 sha.a2
        input `shouldBe` Fx.kInput
        writeGuest m sha.a3 (sha256 input) `shouldReturn` True
        r2 <- resumeFromEcall m (continueWith 0 (shaCost sha.a2))
        r2 `shouldBe` StepEcall
        Just ret <- pendingEcall m
        retGas <- gasRemaining m
        ret.a0 `shouldBe` 6
        Just out <- readGuest m ret.a1 ret.a2
        pure (m, shaGas, retGas, out)
  it "returns sha256(input) with the pinned syscall gas (CompiledSha256ReturnTraceIsExact)" $ do
    (m, shaGas, retGas, out) <- runMethod Fx.kSha256MethodId 10000
    shaGas `shouldBe` 9977
    retGas `shouldBe` 9898
    hex out `shouldBe` Fx.kInputSha256Hex
    -- The executor's Return path: consume the syscall's gas, report the rest.
    consumeGas m kSyscallBaseCost `shouldReturn` True
    gasRemaining m `shouldReturn` (9898 - kSyscallBaseCost)
  it "dispatches the second method to an empty return (CompiledMultiMethodDispatchTraceIsExact)" $ do
    (_, shaGas, retGas, out) <- runMethod Fx.kSha256RepeatedMethodId 10000
    shaGas `shouldBe` 9974
    retGas `shouldBe` 9895
    out `shouldBe` B.empty
  it "traps an unknown method id at canonical UNIMP" $ do
    m <- instantiateContract prepared 0x1234 10000
    start m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapInvalidInstruction
    g <- gasRemaining m
    g < 10000 `shouldBe` True
  it "runs out of gas before the first syscall on a tiny budget" $ do
    m <- instantiateContract prepared Fx.kSha256MethodId 10
    start m `shouldReturn` StepTrap
    trap m `shouldReturn` TrapOutOfGas
    gasRemaining m `shouldReturn` 0
  it "agrees with both models on every budget up to the full run" $ do
    let code = V.fromList (either (error . show) id (mapM specDecode (words32 (B.take (fromIntegral Fx.kCodeBytes) (B.drop 128 Fx.kArtifact)))))
        image = B.replicate (fromIntegral Fx.kInitialMemoryStart) 0 <> Fx.kInput
        mid = fromIntegral (Fx.kSha256MethodId .&. 0xffffffff)
        hiW = fromIntegral (Fx.kSha256MethodId `shiftR` 32)
        host :: ModelHost
        host _ args
          | args.a0 == 0 = ([], continueWith 0 (shaCost args.a2))
          | otherwise = ([], HostHalt 0 0)
    forM_ ([0, 3 .. 200] ++ [9880 .. 10000]) $ \g -> do
      m <- instantiateContract prepared Fx.kSha256MethodId g
      got <- runOutcome host m
      let canon = runCanonical host code (fromIntegral Fx.kMemoryBytes) image [(10, mid), (11, hiW)] 0 g
          threaded = runThreaded 10000 host code (fromIntegral Fx.kMemoryBytes) image [(10, mid), (11, hiW)] g
      (g, got) `shouldBe` (g, canon)
      (g, observable got) `shouldBe` (g, observable threaded)

words32 :: B.ByteString -> [Word32]
words32 bs = [loadU32Le bs i | i <- [0, 4 .. B.length bs - 4]]

-- | What the executor can observe: result, trap, remaining gas, every
-- ecall's arguments and gas, and (unless out of gas) the final state.
observable :: Outcome -> (StepResult, Trap, Word64, [(EcallArgs, Word64)], Maybe ([Word32], B.ByteString, Word32))
observable o =
  ( oResult o
  , oTrap o
  , oGas o
  , oEcalls o
  , if oTrap o == TrapOutOfGas then Nothing else Just (oRegs o, oMem o, oHaltCode o)
  )

------------------------------------------------------------------------------
-- Differential properties

data Program' = Program' [Word32] Word64 [(Int, Word32)] B.ByteString
  deriving (Show)

kDiffMemory :: Int
kDiffMemory = 256

genRandomProgram :: Gen Program'
genRandomProgram = do
  len <- choose (1, 48)
  body <- vectorOf len (genInstr len)
  gas <- frequency [(3, choose (0, 60)), (3, choose (0, 600)), (1, choose (0, 5000))]
  initRegs <- vectorOf 6 ((,) <$> choose (1, 31) <*> genValue)
  image <- B.pack <$> vectorOf kDiffMemory arbitrary
  pure (Program' body gas initRegs image)
  where
    reg' = choose (0, 31) :: Gen Word32
    baseReg = elements [5, 6, 7, 0, 8]
    genValue = frequency [(3, choose (0, 300)), (1, arbitrary), (1, elements [0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 252, 255, 256])]
    genInstr len =
      frequency
        [ (12, (\(f7, f3) rd a b -> rType f7 b a f3 rd) <$> elements rOps <*> reg' <*> reg' <*> reg')
        , (8, (\f3 rd a imm -> iType 0x13 rd f3 a imm) <$> elements [0, 2, 3, 4, 6, 7] <*> reg' <*> reg' <*> choose (-2048, 2047))
        , (3, (\k rd a sh -> iType 0x13 rd (if k == 0 then 1 else 5) a (if k == 2 then 0x400 + sh else sh)) <$> choose (0, 2 :: Int) <*> reg' <*> reg' <*> choose (0, 31))
        , (2, (\o rd imm -> uType o rd imm) <$> elements [0x37, 0x17] <*> reg' <*> arbitrary)
        , (8, (\f3 rd b off -> load rd f3 b off) <$> elements [0, 1, 2, 4, 5] <*> reg' <*> baseReg <*> choose (-8, 264))
        , (6, (\f3 b v off -> store b v off f3) <$> elements [0, 1, 2] <*> baseReg <*> reg' <*> choose (-8, 264))
        , (4, (\f3 a b off -> bType f3 a b (2 * off)) <$> elements [0, 1, 4, 5, 6, 7] <*> reg' <*> reg' <*> choose (-2 * fromIntegral len - 2, 2 * fromIntegral len + 2))
        , (2, (\rd off -> jType rd (2 * off)) <$> reg' <*> choose (-2 * fromIntegral len - 2, 2 * fromIntegral len + 2))
        , (2, (\rd a off -> jalr rd a off) <$> reg' <*> reg' <*> choose (-8, 200))
        , (4, (\a sysno -> [addi 10 0 sysno, kEcall] !! a) <$> choose (0, 1) <*> choose (0, 40))
        , (1, elements [kEbreak, kUnimp, kFence])
        ]
    rOps = [(0, 0), (0x20, 0), (0, 1), (0, 2), (0, 3), (0, 4), (0, 5), (0x20, 5), (0, 6), (0, 7)] ++ [(1, f) | f <- [0 .. 7]]

-- | Host: continue with a value derived from the arguments, charging host
-- gas (sometimes more than remains), occasionally writing guest memory,
-- halting, or reporting out of gas.
diffHost :: ModelHost
diffHost i args
  | args.a0 == 39 = ([], HostHalt (args.a1 + fromIntegral i) 1)
  | args.a0 == 38 = ([(args.a1 .&. 0x1ff, B.pack [fromIntegral i, 0xab])], HostOutOfGas 0)
  | otherwise =
      ( [(args.a2 .&. 0x1ff, B.pack [fromIntegral i, fromIntegral args.a0]) | args.a0 `mod` 3 == 0]
      , continueWith (args.a0 * 7 + fromIntegral i) (fromIntegral (args.a1 .&. 0x3f))
      )

machineOutcome :: Program' -> IO Outcome
machineOutcome (Program' ws gas initRegs image) =
  case preInstantiate (codeFromWords ws) image 0 (fromIntegral kDiffMemory) of
    Left e -> error (show e)
    Right p -> do
      m <- newMachine p gas
      forM_ initRegs (uncurry (setReg m))
      runOutcome diffHost m

specCode :: [Word32] -> V.Vector SInstr
specCode = V.fromList . map (either (error . show) id . specDecode)

differentialSpec :: Spec
differentialSpec = describe "differential (independent models)" $ do
  it "matches the canonical spec model exactly on random programs" $
    withNumTests 3000 $
      forAll genRandomProgram $ \prog@(Program' ws gas initRegs image) -> ioProperty $ do
        got <- machineOutcome prog
        let want = runCanonical diffHost (specCode ws) kDiffMemory image initRegs 0 gas
        pure (counterexample (show (got, want)) (got == want))
  it "matches production run-based metering on random programs (all chunk caps)" $
    withNumTests 3000 $
      forAll genRandomProgram $ \prog@(Program' ws gas initRegs image) -> forAll (elements [3, 4, 5, 6, 7, 9, 13, 10000]) $ \cap' -> ioProperty $ do
        got <- machineOutcome prog
        let want = runThreaded cap' diffHost (specCode ws) kDiffMemory image initRegs gas
        pure (counterexample (show (cap', observable got, observable want)) (observable got == observable want))
  it "matches production metering for every budget of fixed fault/ecall programs" $ do
    let progs =
          [ [addi 5 0 100, addi 6 0 5, load 4 2 5 200, op 7 6 6 0 0, op 8 6 6 0 0, kEcall, addi 1 10 1, kEcall]
          , [addi 10 0 3, kEcall, addi 5 0 252, store 5 10 0 2, store 5 10 4 2, addi 1 1 1, kEcall]
          , [addi 1 0 3, addi 2 2 1, addi 1 1 (-1), branch 1 1 0 (-8), addi 10 0 39, kEcall]
          , replicate 9 (op 3 1 2 4 1) ++ [load 4 2 0 300, kEcall]
          , [addi 5 0 250, load 4 2 5 0, load 4 2 5 4, kFence, kEcall, jType 0 (-8)]
          ]
    forM_ progs $ \ws -> forM_ [3, 4, 5, 7, 10000] $ \cap' -> forM_ [0 .. 80] $ \g -> do
      got <- machineOutcome (Program' ws g [] (B.replicate kDiffMemory 0))
      let want = runThreaded cap' diffHost (specCode ws) kDiffMemory (B.replicate kDiffMemory 0) [] g
      (ws, cap', g, observable got) `shouldBe` (ws, cap', g, observable want)
  it "uses only valid words in the random generator" $
    property $ forAll genRandomProgram $ \(Program' ws _ _ _) -> all (isRight . decodeInstruction) ws
  it "covers traps, ecalls and halts in the random corpus" $ do
    outcomes <- mapM machineOutcome =<< generate (vectorOf 400 genRandomProgram)
    let traps = nub [oTrap o | o <- outcomes]
    unless (all (`elem` traps) [TrapBadPc, TrapBadMemory, TrapOutOfGas]) $
      expectationFailure ("random corpus trap coverage too low: " ++ show traps)
    any (not . null . oEcalls) outcomes `shouldBe` True
