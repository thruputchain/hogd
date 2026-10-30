-- | Operand/result vectors adapted from selected upstream riscv-tests RV32UI
-- wrappers, their included RV64UI bodies, and RV32UM assembly tests, as
-- carried by @rv32_borrowed_conformance_test.cc@ (see
-- @hogd/hog_exec/runtime/rv32/riscv-tests-license.md@), plus the core
-- conformance cases of @rv32_conformance_test.cc@.
module Rv32Vectors
  ( RrVector (..)
  , rrVectors
  , ImmVector (..)
  , immVectors
  , BranchVector (..)
  , branchVectors
  , LoadVector (..)
  , loadVectors
  , StoreVector (..)
  , storeVectors
  , UpperVector (..)
  , upperVectors
  , JumpVector (..)
  , jumpVectors
  , kDataAddress
  , ConformanceCase (..)
  , conformanceCase
  , conformanceCases
  ) where

import Data.Int (Int32)
import Data.Word (Word32, Word64, Word8)
import Hogd.Exec.Runtime.Rv32.Host
import Hogd.Exec.Runtime.Rv32.Program
import Rv32Support

kDataAddress :: Word32
kDataAddress = 64

-- | (name, funct7, funct3, lhs, rhs, expected)
data RrVector = RrVector String Word32 Word32 Word32 Word32 Word32

rrVectors :: [RrVector]
rrVectors =
  [ v "add_wrap_positive" add 0x7fffffff 0x00007fff 0x80007ffe
  , v "add_negative_plus_one" add 0xffffffff 0x00000001 0x00000000
  , v "sub_underflow" sub 0x00000000 0x00007fff 0xffff8001
  , v "sub_min_minus_small" sub 0x80000000 0x00007fff 0x7fff8001
  , v "and_checkerboard" and' 0xff00ff00 0x0f0f0f0f 0x0f000f00
  , v "or_checkerboard" or' 0x0ff00ff0 0xf0f0f0f0 0xfff0fff0
  , v "xor_checkerboard" xor' 0x00ff00ff 0x0f0f0f0f 0x0ff00ff0
  , v "slt_min_less_zero" slt 0x80000000 0x00000000 0x00000001
  , v "slt_unsigned_max_not_less_one" sltu 0xffffffff 0x00000001 0x00000000
  , v "sll_masks_shift_amount" sll 0x21212121 0xffffffff 0x80000000
  , v "srl_masks_shift_amount" srl 0x21212121 0xffffffff 0x00000000
  , v "sra_negative_fill" sra 0x81818181 0x0000001f 0xffffffff
  , v "mul_low_wrap" mul 0xaaaaaaab 0x0002fe7d 0x0000ff7f
  , v "mulh_signed_high" mulh 0xaaaaaaab 0x0002fe7d 0xffff0081
  , v "mulhsu_mixed_high" mulhsu 0x80000000 0xffff8000 0x80004000
  , v "mulhu_unsigned_high" mulhu 0xff000000 0xff000000 0xfe010000
  , v "div_signed_negative" div' 0xffffffec 0x00000006 0xfffffffd
  , v "div_signed_overflow" div' 0x80000000 0xffffffff 0x80000000
  , v "div_signed_by_zero" div' 0x00000001 0x00000000 0xffffffff
  , v "divu_large" divu 0xffffffec 0x00000006 0x2aaaaaa7
  , v "divu_by_zero" divu 0x80000000 0x00000000 0xffffffff
  , v "rem_signed_negative" rem' 0xffffffec 0x00000006 0xfffffffe
  , v "rem_signed_overflow" rem' 0x80000000 0xffffffff 0x00000000
  , v "rem_signed_by_zero" rem' 0x80000000 0x00000000 0x80000000
  , v "remu_large" remu 0xffffffec 0xfffffffa 0xffffffec
  , v "remu_by_zero" remu 0x80000000 0x00000000 0x80000000
  ]
  where
    v name (f7, f3) = RrVector name f7 f3
    add = (0x00, 0)
    sub = (0x20, 0)
    sll = (0x00, 1)
    slt = (0x00, 2)
    sltu = (0x00, 3)
    xor' = (0x00, 4)
    srl = (0x00, 5)
    sra = (0x20, 5)
    or' = (0x00, 6)
    and' = (0x00, 7)
    mul = (0x01, 0)
    mulh = (0x01, 1)
    mulhsu = (0x01, 2)
    mulhu = (0x01, 3)
    div' = (0x01, 4)
    divu = (0x01, 5)
    rem' = (0x01, 6)
    remu = (0x01, 7)

-- | (name, funct3, imm bits, lhs, expected)
data ImmVector = ImmVector String Word32 Int32 Word32 Word32

immVectors :: [ImmVector]
immVectors =
  [ ImmVector "addi_sign_extreme" 0 0x800 0x80000000 0x7ffff800
  , ImmVector "addi_wrap" 0 0x001 0x7fffffff 0x80000000
  , ImmVector "slti_min_less_zero" 2 0x000 0x80000000 1
  , ImmVector "sltiu_zero_less_negative_imm" 3 0x800 0x00000000 1
  , ImmVector "xori_sign_extended_imm" 4 0xf0f 0x00ff0f00 0xff00f00f
  , ImmVector "ori_sign_extended_imm" 6 0xf0f 0xff00ff00 0xffffff0f
  , ImmVector "andi_sign_extended_imm" 7 0xf0f 0xff00ff00 0xff00ff00
  , ImmVector "slli_top_bit" 1 31 0x00000001 0x80000000
  , ImmVector "srli_top_bit" 5 31 0x80000001 0x00000001
  , ImmVector "srai_top_bit" 5 (0x400 + 31) 0x80000001 0xffffffff
  ]

-- | (name, funct3, lhs, rhs, taken)
data BranchVector = BranchVector String Word32 Word32 Word32 Bool

branchVectors :: [BranchVector]
branchVectors =
  [ BranchVector "beq_taken" 0 0x00000001 0x00000001 True
  , BranchVector "beq_zero_taken" 0 0x00000000 0x00000000 True
  , BranchVector "beq_negative_taken" 0 0xffffffff 0xffffffff True
  , BranchVector "beq_not_taken" 0 0x00000001 0x00000002 False
  , BranchVector "beq_zero_one_not_taken" 0 0x00000000 0x00000001 False
  , BranchVector "beq_negative_positive_not_taken" 0 0xffffffff 0x00000001 False
  , BranchVector "beq_positive_negative_not_taken" 0 0x00000001 0xffffffff False
  , BranchVector "bne_taken" 1 0x00000001 0x00000002 True
  , BranchVector "bne_zero_one_taken" 1 0x00000000 0x00000001 True
  , BranchVector "bne_one_zero_taken" 1 0x00000001 0x00000000 True
  , BranchVector "bne_negative_positive_taken" 1 0xffffffff 0x00000001 True
  , BranchVector "bne_positive_negative_taken" 1 0x00000001 0xffffffff True
  , BranchVector "bne_not_taken" 1 0x00000001 0x00000001 False
  , BranchVector "bne_zero_not_taken" 1 0x00000000 0x00000000 False
  , BranchVector "bne_negative_not_taken" 1 0xffffffff 0xffffffff False
  , BranchVector "blt_signed_min_taken" 4 0x80000000 0x00000000 True
  , BranchVector "blt_zero_one_taken" 4 0x00000000 0x00000001 True
  , BranchVector "blt_negative_one_taken" 4 0xffffffff 0x00000001 True
  , BranchVector "blt_negative_order_taken" 4 0xfffffffe 0xffffffff True
  , BranchVector "blt_signed_positive_not_taken" 4 0x00000007 0x00000003 False
  , BranchVector "blt_one_zero_not_taken" 4 0x00000001 0x00000000 False
  , BranchVector "blt_positive_negative_not_taken" 4 0x00000001 0xffffffff False
  , BranchVector "blt_negative_order_not_taken" 4 0xffffffff 0xfffffffe False
  , BranchVector "blt_one_minus_two_not_taken" 4 0x00000001 0xfffffffe False
  , BranchVector "bge_signed_negative_not_taken" 5 0xffffffff 0x00000001 False
  , BranchVector "bge_signed_equal_taken" 5 0xffffffff 0xffffffff True
  , BranchVector "bge_zero_equal_taken" 5 0x00000000 0x00000000 True
  , BranchVector "bge_one_equal_taken" 5 0x00000001 0x00000001 True
  , BranchVector "bge_one_zero_taken" 5 0x00000001 0x00000000 True
  , BranchVector "bge_one_negative_taken" 5 0x00000001 0xffffffff True
  , BranchVector "bge_negative_order_taken" 5 0xffffffff 0xfffffffe True
  , BranchVector "bge_zero_one_not_taken" 5 0x00000000 0x00000001 False
  , BranchVector "bge_negative_order_not_taken" 5 0xfffffffe 0xffffffff False
  , BranchVector "bge_minus_two_one_not_taken" 5 0xfffffffe 0x00000001 False
  , BranchVector "bltu_zero_less_max_taken" 6 0x00000000 0xffffffff True
  , BranchVector "bltu_zero_one_taken" 6 0x00000000 0x00000001 True
  , BranchVector "bltu_max_minus_one_taken" 6 0xfffffffe 0xffffffff True
  , BranchVector "bltu_max_less_one_not_taken" 6 0xffffffff 0x00000001 False
  , BranchVector "bltu_one_zero_not_taken" 6 0x00000001 0x00000000 False
  , BranchVector "bltu_max_order_not_taken" 6 0xffffffff 0xfffffffe False
  , BranchVector "bltu_max_zero_not_taken" 6 0xffffffff 0x00000000 False
  , BranchVector "bltu_high_bit_not_taken" 6 0x80000000 0x7fffffff False
  , BranchVector "bgeu_max_greater_one_taken" 7 0xffffffff 0x00000001 True
  , BranchVector "bgeu_zero_equal_taken" 7 0x00000000 0x00000000 True
  , BranchVector "bgeu_one_equal_taken" 7 0x00000001 0x00000001 True
  , BranchVector "bgeu_max_equal_taken" 7 0xffffffff 0xffffffff True
  , BranchVector "bgeu_one_zero_taken" 7 0x00000001 0x00000000 True
  , BranchVector "bgeu_max_order_taken" 7 0xffffffff 0xfffffffe True
  , BranchVector "bgeu_max_zero_taken" 7 0xffffffff 0x00000000 True
  , BranchVector "bgeu_zero_greater_max_not_taken" 7 0x00000000 0xffffffff False
  , BranchVector "bgeu_zero_one_not_taken" 7 0x00000000 0x00000001 False
  , BranchVector "bgeu_max_minus_one_not_taken" 7 0xfffffffe 0xffffffff False
  , BranchVector "bgeu_high_bit_not_taken" 7 0x7fffffff 0x80000000 False
  ]

-- | (name, funct3, memory at kDataAddress, address, offset, expected)
data LoadVector = LoadVector String Word32 [Word8] Word32 Int32 Word32

loadVectors :: [LoadVector]
loadVectors =
  [ LoadVector "lb_byte_ff" 0 b4 kDataAddress 0 0xffffffff
  , LoadVector "lb_byte_zero" 0 b4 (kDataAddress + 1) 1 0
  , LoadVector "lb_byte_f0" 0 b4 (kDataAddress + 2) 2 0xfffffff0
  , LoadVector "lb_negative_offset" 0 b4 kDataAddress (-3) 0xffffffff
  , LoadVector "lbu_byte_ff" 4 b4 kDataAddress 0 0xff
  , LoadVector "lbu_byte_f0" 4 b4 (kDataAddress + 2) 2 0xf0
  , LoadVector "lbu_negative_offset" 4 b4 kDataAddress (-3) 0xff
  , LoadVector "lh_positive" 1 h8 kDataAddress 0 0x000000ff
  , LoadVector "lh_negative" 1 h8 (kDataAddress + 2) 2 0xffffff00
  , LoadVector "lh_negative_offset" 1 h8 (kDataAddress + 4) (-2) 0x00000ff0
  , LoadVector "lhu_positive" 5 h8 kDataAddress 0 0x000000ff
  , LoadVector "lhu_unsigned" 5 h8 (kDataAddress + 2) 2 0x0000ff00
  , LoadVector "lhu_negative_offset" 5 h8 (kDataAddress + 4) (-2) 0x00000ff0
  , LoadVector "lw_positive" 2 w16 kDataAddress 0 0x00ff00ff
  , LoadVector "lw_negative" 2 w16 (kDataAddress + 4) 4 0xff00ff00
  , LoadVector "lw_negative_offset" 2 w16 (kDataAddress + 8) (-4) 0x0ff00ff0
  ]
  where
    b4 = [0xff, 0x00, 0xf0, 0x0f]
    h8 = [0xff, 0x00, 0x00, 0xff, 0xf0, 0x0f, 0x0f, 0xf0]
    w16 = [0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0xff, 0xf0, 0x0f, 0xf0, 0x0f, 0x0f, 0xf0, 0x0f, 0xf0]

-- | (name, funct3, value, address, offset, expected bytes at address)
data StoreVector = StoreVector String Word32 Word32 Word32 Int32 [Word8]

storeVectors :: [StoreVector]
storeVectors =
  [ StoreVector "sb_low_byte" 0 0x123456aa kDataAddress 0 [0xaa]
  , StoreVector "sb_zero_byte" 0 0 (kDataAddress + 1) 1 [0]
  , StoreVector "sb_negative_offset" 0 0x123456aa kDataAddress (-3) [0xaa]
  , StoreVector "sh_low_half" 1 0x123400aa kDataAddress 0 [0xaa, 0x00]
  , StoreVector "sh_high_low_half" 1 0x1234aa00 (kDataAddress + 2) 2 [0x00, 0xaa]
  , StoreVector "sh_negative_offset" 1 0x12340aa0 (kDataAddress + 4) (-2) [0xa0, 0x0a]
  , StoreVector "sw_low_word" 2 0x00aa00aa kDataAddress 0 [0xaa, 0x00, 0xaa, 0x00]
  , StoreVector "sw_high_word" 2 0xaa00aa00 (kDataAddress + 4) 4 [0x00, 0xaa, 0x00, 0xaa]
  , StoreVector "sw_negative_offset" 2 0x0aa00aa0 (kDataAddress + 8) (-4) [0xa0, 0x0a, 0xa0, 0x0a]
  ]

-- | (name, opcode, imm, initial pc, expected)
data UpperVector = UpperVector String Word32 Word32 Word32 Word32

upperVectors :: [UpperVector]
upperVectors =
  [ UpperVector "lui_zero" 0x37 0x00000000 0 0x00000000
  , UpperVector "lui_negative" 0x37 0xfffff000 0 0xfffff000
  , UpperVector "lui_sign_bit" 0x37 0x80000000 0 0x80000000
  , UpperVector "auipc_positive" 0x17 0x00002000 4 0x00002004
  , UpperVector "auipc_negative" 0x17 0xffffe000 8 0xffffe008
  ]

-- | (name, first instruction, initial x1, link reg, expected link, marker)
data JumpVector = JumpVector String Word32 Word32 Int Word32 Word32

jumpVectors :: [JumpVector]
jumpVectors =
  [ JumpVector "jal_skips_delay_slot_like_code" (jType 4 12) 0 4 4 0x33
  , JumpVector "jal_x0_discards_link" (jType 0 12) 0 0 0 0x33
  , JumpVector "jalr_basic" (jalr 4 1 0) 12 4 4 0x33
  , JumpVector "jalr_masks_low_target_bit" (jalr 4 1 1) 11 4 4 0x33
  , JumpVector "jalr_rd_equals_rs1" (jalr 1 1 0) 12 1 4 0x33
  ]

-- | One @rv32_conformance_test.cc@ case, run to a terminal result with a
-- host that halts with the guest's a0.
data ConformanceCase = ConformanceCase
  { ccName :: String
  , ccCode :: [Word32]
  , ccMemoryBytes :: Word32
  , ccGasLimit :: Word64
  , ccInitialPc :: Word32
  , ccInitialRegs :: [(Int, Word32)]
  , ccInitialMemory :: [(Word32, [Word8])]
  , ccExpectedResult :: StepResult
  , ccExpectedTrap :: Trap
  , ccExpectedHaltCode :: Word32
  , ccExpectedPc :: Maybe Word32
  , ccExpectedGasConsumed :: Maybe Word64
  , ccExpectedRegs :: [(Int, Word32)]
  , ccExpectedMemory :: [(Word32, [Word8])]
  }

conformanceCase :: String -> [Word32] -> ConformanceCase
conformanceCase name code =
  ConformanceCase
    { ccName = name
    , ccCode = code
    , ccMemoryBytes = 256
    , ccGasLimit = 1000
    , ccInitialPc = 0
    , ccInitialRegs = []
    , ccInitialMemory = []
    , ccExpectedResult = StepHalted
    , ccExpectedTrap = TrapNone
    , ccExpectedHaltCode = 0
    , ccExpectedPc = Nothing
    , ccExpectedGasConsumed = Nothing
    , ccExpectedRegs = []
    , ccExpectedMemory = []
    }

conformanceCases :: [ConformanceCase]
conformanceCases =
  [ (conformanceCase "arithmetic_and_logic" [addi 1 0 7, addi 2 0 (-3), rType 0 2 1 0 3, rType 0x20 2 1 0 4, rType 0 2 1 2 5, rType 0 2 1 3 6, rType 0 2 1 4 7, rType 0 2 1 6 8, rType 0 2 1 7 9, kEcall])
      { ccExpectedPc = Just 40
      , ccExpectedGasConsumed = Just 10
      , ccExpectedRegs = [(3, 4), (4, 10), (5, 0), (6, 1), (7, 0xfffffffa), (8, 0xffffffff), (9, 5)]
      }
  , (conformanceCase "shift_immediates_and_registers" [addi 1 0 (-8), addi 2 0 2, iType 0x13 3 1 1 1, iType 0x13 4 5 1 1, iType 0x13 5 5 1 0x401, rType 0 2 1 1 6, rType 0 2 1 5 7, rType 0x20 2 1 5 8, addi 9 0 (-32), rType 0 9 1 1 13, rType 0 9 1 5 14, rType 0x20 9 1 5 15, kEcall])
      { ccExpectedPc = Just 52
      , ccExpectedRegs = [(3, 0xfffffff0), (4, 0x7ffffffc), (5, 0xfffffffc), (6, 0xffffffe0), (7, 0x3ffffffe), (8, 0xfffffffe), (13, 0xfffffff8), (14, 0xfffffff8), (15, 0xfffffff8)]
      }
  , (conformanceCase "branch_jump_and_upper_immediates" [addi 1 0 1, bType 0 1 1 8, addi 2 0 99, jType 3 8, addi 2 0 2, uType 0x17 4 0x00001000, kEcall])
      { ccInitialRegs = [(2, 0)]
      , ccExpectedPc = Just 28
      , ccExpectedRegs = [(2, 0), (3, 16), (4, 0x00001014)]
      }
  , (conformanceCase "little_endian_load_store_sign_extension" [addi 1 0 32, uType 0x37 2 0xdeadc000, addi 2 2 (-273), sType 2 1 2 0, load 3 0 1 0, load 4 4 1 0, load 5 1 1 0, load 6 5 1 0, load 7 2 1 0, kEcall])
      { ccExpectedPc = Just 40
      , ccExpectedRegs = [(3, 0xffffffef), (4, 0x000000ef), (5, 0xffffbeef), (6, 0x0000beef), (7, 0xdeadbeef)]
      , ccExpectedMemory = [(32, [0xef, 0xbe, 0xad, 0xde])]
      }
  , (conformanceCase "rv32m_division_edge_cases" [addi 1 0 (-7), addi 2 0 3, rType 1 2 1 0 3, rType 1 2 1 4 4, rType 1 2 1 6 5, rType 1 0 1 4 6, rType 1 0 1 6 7, uType 0x37 8 0x80000000, addi 9 0 (-1), rType 1 9 8 4 10, rType 1 9 8 6 11, rType 1 0 1 5 12, rType 1 0 1 7 13, kEcall])
      { ccExpectedHaltCode = 0x80000000
      , ccExpectedPc = Just 56
      , ccExpectedRegs = [(3, 0xffffffeb), (4, 0xfffffffe), (5, 0xffffffff), (6, 0xffffffff), (7, 0xfffffff9), (10, 0x80000000), (11, 0), (12, 0xffffffff), (13, 0xfffffff9)]
      }
  , (conformanceCase "jalr_masks_low_target_bit" [addi 1 0 9, jalr 2 1 0, kEcall])
      { ccExpectedPc = Just 12
      , ccExpectedRegs = [(2, 8)]
      }
  , (conformanceCase "fence_is_ordering_noop_in_single_threaded_vm" [addi 1 0 1, kFence, addi 2 1 2, kEcall])
      { ccExpectedPc = Just 16
      , ccExpectedGasConsumed = Just 4
      , ccExpectedRegs = [(1, 1), (2, 3)]
      }
  , (conformanceCase "ebreak_is_deterministic_runtime_trap" [addi 1 0 7, kEbreak, addi 1 0 9])
      { ccExpectedResult = StepTrap
      , ccExpectedTrap = TrapBreakpoint
      , ccExpectedPc = Just 4
      , ccExpectedGasConsumed = Just 2
      , ccExpectedRegs = [(1, 7)]
      }
  , (conformanceCase "unimp_is_deterministic_invalid_instruction_trap" [addi 1 0 7, kUnimp, addi 1 0 9])
      { ccExpectedResult = StepTrap
      , ccExpectedTrap = TrapInvalidInstruction
      , ccExpectedPc = Just 4
      , ccExpectedGasConsumed = Just 2
      , ccExpectedRegs = [(1, 7)]
      }
  , (conformanceCase "unaligned_load_reassembles_little_endian" [addi 1 0 32, uType 0x37 2 0xdeadc000, addi 2 2 (-273), sType 2 1 2 0, addi 3 1 1, load 4 2 3 0, kEcall])
      { ccExpectedPc = Just 28
      , ccExpectedGasConsumed = Just (6 * kBaseInstructionGas + kWideLoadGas)
      , ccExpectedRegs = [(4, 0x00deadbe)]
      , ccExpectedMemory = [(32, [0xef, 0xbe, 0xad, 0xde])]
      }
  , (conformanceCase "unaligned_store_mutates_exactly_four_bytes" [addi 1 0 33, uType 0x37 2 0xdeadc000, addi 2 2 (-273), sType 2 1 2 0, kEcall])
      { ccInitialMemory = [(32, [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11])]
      , ccExpectedPc = Just 20
      , ccExpectedGasConsumed = Just 5
      , ccExpectedMemory = [(32, [0x11, 0xef, 0xbe, 0xad, 0xde, 0x11, 0x11, 0x11])]
      }
  , (conformanceCase "unaligned_out_of_bounds_load_traps_bad_memory" [addi 1 0 253, load 2 2 1 0, kEcall])
      { ccInitialRegs = [(2, 0)]
      , ccExpectedResult = StepTrap
      , ccExpectedTrap = TrapBadMemory
      , ccExpectedPc = Just 4
      , ccExpectedGasConsumed = Just (kBaseInstructionGas + kWideLoadGas)
      , ccExpectedRegs = [(2, 0)]
      }
  , (conformanceCase "taken_branch_to_bad_pc_traps_after_gas" [addi 1 0 1, bType 0 1 1 12, addi 2 0 1])
      { ccInitialRegs = [(2, 0)]
      , ccExpectedResult = StepTrap
      , ccExpectedTrap = TrapBadPc
      , ccExpectedPc = Just 4
      , ccExpectedGasConsumed = Just (kBaseInstructionGas + kBranchInstructionGas)
      , ccExpectedRegs = [(2, 0)]
      }
  , (conformanceCase "out_of_gas_has_no_partial_mutation" [addi 1 0 1, kEcall])
      { ccGasLimit = 0
      , ccExpectedResult = StepTrap
      , ccExpectedTrap = TrapOutOfGas
      , ccExpectedPc = Just 0
      , ccExpectedGasConsumed = Just 0
      , ccExpectedRegs = [(1, 0)]
      }
  ]
