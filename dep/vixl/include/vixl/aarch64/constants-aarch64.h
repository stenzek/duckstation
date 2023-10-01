// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_AARCH64_CONSTANTS_AARCH64_H_
#define VIXL_AARCH64_CONSTANTS_AARCH64_H_

#include "../globals-vixl.h"

namespace vixl {
namespace aarch64 {

const unsigned kNumberOfRegisters = 32;
const unsigned kNumberOfVRegisters = 32;
const unsigned kNumberOfFPRegisters = kNumberOfVRegisters;
// Callee saved registers are x21-x30(lr).
const int kNumberOfCalleeSavedRegisters = 10;
const int kFirstCalleeSavedRegisterIndex = 21;
// Callee saved FP registers are d8-d15.
const int kNumberOfCalleeSavedFPRegisters = 8;
const int kFirstCalleeSavedFPRegisterIndex = 8;

// clang-format off
#define AARCH64_REGISTER_CODE_LIST(R)                                          \
  R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7)                               \
  R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15)                              \
  R(16) R(17) R(18) R(19) R(20) R(21) R(22) R(23)                              \
  R(24) R(25) R(26) R(27) R(28) R(29) R(30) R(31)

#define INSTRUCTION_FIELDS_LIST(V_)                                          \
/* Register fields */                                                        \
V_(Rd, 4, 0, ExtractBits)                 /* Destination register.        */ \
V_(Rn, 9, 5, ExtractBits)                 /* First source register.       */ \
V_(Rm, 20, 16, ExtractBits)               /* Second source register.      */ \
V_(Ra, 14, 10, ExtractBits)               /* Third source register.       */ \
V_(Rt, 4, 0, ExtractBits)                 /* Load/store register.         */ \
V_(Rt2, 14, 10, ExtractBits)              /* Load/store second register.  */ \
V_(Rs, 20, 16, ExtractBits)               /* Exclusive access status.     */ \
                                                                             \
/* Common bits */                                                            \
V_(SixtyFourBits, 31, 31, ExtractBits)                                       \
V_(FlagsUpdate, 29, 29, ExtractBits)                                         \
                                                                             \
/* PC relative addressing */                                                 \
V_(ImmPCRelHi, 23, 5, ExtractSignedBits)                                     \
V_(ImmPCRelLo, 30, 29, ExtractBits)                                          \
                                                                             \
/* Add/subtract/logical shift register */                                    \
V_(ShiftDP, 23, 22, ExtractBits)                                             \
V_(ImmDPShift, 15, 10, ExtractBits)                                          \
                                                                             \
/* Add/subtract immediate */                                                 \
V_(ImmAddSub, 21, 10, ExtractBits)                                           \
V_(ShiftAddSub, 23, 22, ExtractBits)                                         \
                                                                             \
/* Add/substract extend */                                                   \
V_(ImmExtendShift, 12, 10, ExtractBits)                                      \
V_(ExtendMode, 15, 13, ExtractBits)                                          \
                                                                             \
/* Move wide */                                                              \
V_(ImmMoveWide, 20, 5, ExtractBits)                                          \
V_(ShiftMoveWide, 22, 21, ExtractBits)                                       \
                                                                             \
/* Logical immediate, bitfield and extract */                                \
V_(BitN, 22, 22, ExtractBits)                                                \
V_(ImmRotate, 21, 16, ExtractBits)                                           \
V_(ImmSetBits, 15, 10, ExtractBits)                                          \
V_(ImmR, 21, 16, ExtractBits)                                                \
V_(ImmS, 15, 10, ExtractBits)                                                \
                                                                             \
/* Test and branch immediate */                                              \
V_(ImmTestBranch, 18, 5, ExtractSignedBits)                                  \
V_(ImmTestBranchBit40, 23, 19, ExtractBits)                                  \
V_(ImmTestBranchBit5, 31, 31, ExtractBits)                                   \
                                                                             \
/* Conditionals */                                                           \
V_(Condition, 15, 12, ExtractBits)                                           \
V_(ConditionBranch, 3, 0, ExtractBits)                                       \
V_(Nzcv, 3, 0, ExtractBits)                                                  \
V_(ImmCondCmp, 20, 16, ExtractBits)                                          \
V_(ImmCondBranch, 23, 5, ExtractSignedBits)                                  \
                                                                             \
/* Floating point */                                                         \
V_(FPType, 23, 22, ExtractBits)                                              \
V_(ImmFP, 20, 13, ExtractBits)                                               \
V_(FPScale, 15, 10, ExtractBits)                                             \
                                                                             \
/* Load Store */                                                             \
V_(ImmLS, 20, 12, ExtractSignedBits)                                         \
V_(ImmLSUnsigned, 21, 10, ExtractBits)                                       \
V_(ImmLSPair, 21, 15, ExtractSignedBits)                                     \
V_(ImmShiftLS, 12, 12, ExtractBits)                                          \
V_(LSOpc, 23, 22, ExtractBits)                                               \
V_(LSVector, 26, 26, ExtractBits)                                            \
V_(LSSize, 31, 30, ExtractBits)                                              \
V_(ImmPrefetchOperation, 4, 0, ExtractBits)                                  \
V_(PrefetchHint, 4, 3, ExtractBits)                                          \
V_(PrefetchTarget, 2, 1, ExtractBits)                                        \
V_(PrefetchStream, 0, 0, ExtractBits)                                        \
                                                                             \
/* Other immediates */                                                       \
V_(ImmUncondBranch, 25, 0, ExtractSignedBits)                                \
V_(ImmCmpBranch, 23, 5, ExtractSignedBits)                                   \
V_(ImmLLiteral, 23, 5, ExtractSignedBits)                                    \
V_(ImmException, 20, 5, ExtractBits)                                         \
V_(ImmHint, 11, 5, ExtractBits)                                              \
V_(ImmBarrierDomain, 11, 10, ExtractBits)                                    \
V_(ImmBarrierType, 9, 8, ExtractBits)                                        \
                                                                             \
/* System (MRS, MSR, SYS) */                                                 \
V_(ImmSystemRegister, 20, 5, ExtractBits)                                    \
V_(SysO0, 19, 19, ExtractBits)                                               \
V_(SysOp, 18, 5, ExtractBits)                                                \
V_(SysOp0, 20, 19, ExtractBits)                                              \
V_(SysOp1, 18, 16, ExtractBits)                                              \
V_(SysOp2, 7, 5, ExtractBits)                                                \
V_(CRn, 15, 12, ExtractBits)                                                 \
V_(CRm, 11, 8, ExtractBits)                                                  \
                                                                             \
/* Load-/store-exclusive */                                                  \
V_(LdStXLoad, 22, 22, ExtractBits)                                           \
V_(LdStXNotExclusive, 23, 23, ExtractBits)                                   \
V_(LdStXAcquireRelease, 15, 15, ExtractBits)                                 \
V_(LdStXSizeLog2, 31, 30, ExtractBits)                                       \
V_(LdStXPair, 21, 21, ExtractBits)                                           \
                                                                             \
/* NEON generic fields */                                                    \
V_(NEONQ, 30, 30, ExtractBits)                                               \
V_(NEONSize, 23, 22, ExtractBits)                                            \
V_(NEONLSSize, 11, 10, ExtractBits)                                          \
V_(NEONS, 12, 12, ExtractBits)                                               \
V_(NEONL, 21, 21, ExtractBits)                                               \
V_(NEONM, 20, 20, ExtractBits)                                               \
V_(NEONH, 11, 11, ExtractBits)                                               \
V_(ImmNEONExt, 14, 11, ExtractBits)                                          \
V_(ImmNEON5, 20, 16, ExtractBits)                                            \
V_(ImmNEON4, 14, 11, ExtractBits)                                            \
                                                                             \
/* NEON extra fields */                                                      \
V_(ImmRotFcadd, 12, 12, ExtractBits)                                         \
V_(ImmRotFcmlaVec, 12, 11, ExtractBits)                                      \
V_(ImmRotFcmlaSca, 14, 13, ExtractBits)                                      \
                                                                             \
/* NEON Modified Immediate fields */                                         \
V_(ImmNEONabc, 18, 16, ExtractBits)                                          \
V_(ImmNEONdefgh, 9, 5, ExtractBits)                                          \
V_(NEONModImmOp, 29, 29, ExtractBits)                                        \
V_(NEONCmode, 15, 12, ExtractBits)                                           \
                                                                             \
/* NEON Shift Immediate fields */                                            \
V_(ImmNEONImmhImmb, 22, 16, ExtractBits)                                     \
V_(ImmNEONImmh, 22, 19, ExtractBits)                                         \
V_(ImmNEONImmb, 18, 16, ExtractBits)
// clang-format on

#define SYSTEM_REGISTER_FIELDS_LIST(V_, M_) \
  /* NZCV */                                \
  V_(Flags, 31, 28, ExtractBits)            \
  V_(N, 31, 31, ExtractBits)                \
  V_(Z, 30, 30, ExtractBits)                \
  V_(C, 29, 29, ExtractBits)                \
  V_(V, 28, 28, ExtractBits)                \
  M_(NZCV, Flags_mask)                      \
  /* FPCR */                                \
  V_(AHP, 26, 26, ExtractBits)              \
  V_(DN, 25, 25, ExtractBits)               \
  V_(FZ, 24, 24, ExtractBits)               \
  V_(RMode, 23, 22, ExtractBits)            \
  M_(FPCR, AHP_mask | DN_mask | FZ_mask | RMode_mask)

// Fields offsets.
#define DECLARE_FIELDS_OFFSETS(Name, HighBit, LowBit, X) \
  const int Name##_offset = LowBit;                      \
  const int Name##_width = HighBit - LowBit + 1;         \
  const uint32_t Name##_mask = ((1 << Name##_width) - 1) << LowBit;
#define NOTHING(A, B)
INSTRUCTION_FIELDS_LIST(DECLARE_FIELDS_OFFSETS)
SYSTEM_REGISTER_FIELDS_LIST(DECLARE_FIELDS_OFFSETS, NOTHING)
#undef NOTHING
#undef DECLARE_FIELDS_BITS

// ImmPCRel is a compound field (not present in INSTRUCTION_FIELDS_LIST), formed
// from ImmPCRelLo and ImmPCRelHi.
const int ImmPCRel_mask = ImmPCRelLo_mask | ImmPCRelHi_mask;

// Disable `clang-format` for the `enum`s below. We care about the manual
// formatting that `clang-format` would destroy.
// clang-format off

// Condition codes.
enum Condition {
  eq = 0,   // Z set            Equal.
  ne = 1,   // Z clear          Not equal.
  cs = 2,   // C set            Carry set.
  cc = 3,   // C clear          Carry clear.
  mi = 4,   // N set            Negative.
  pl = 5,   // N clear          Positive or zero.
  vs = 6,   // V set            Overflow.
  vc = 7,   // V clear          No overflow.
  hi = 8,   // C set, Z clear   Unsigned higher.
  ls = 9,   // C clear or Z set Unsigned lower or same.
  ge = 10,  // N == V           Greater or equal.
  lt = 11,  // N != V           Less than.
  gt = 12,  // Z clear, N == V  Greater than.
  le = 13,  // Z set or N != V  Less then or equal
  al = 14,  //                  Always.
  nv = 15,  // Behaves as always/al.

  // Aliases.
  hs = cs,  // C set            Unsigned higher or same.
  lo = cc   // C clear          Unsigned lower.
};

inline Condition InvertCondition(Condition cond) {
  // Conditions al and nv behave identically, as "always true". They can't be
  // inverted, because there is no "always false" condition.
  VIXL_ASSERT((cond != al) && (cond != nv));
  return static_cast<Condition>(cond ^ 1);
}

enum FPTrapFlags {
  EnableTrap   = 1,
  DisableTrap = 0
};

enum FlagsUpdate {
  SetFlags   = 1,
  LeaveFlags = 0
};

enum StatusFlags {
  NoFlag    = 0,

  // Derive the flag combinations from the system register bit descriptions.
  NFlag     = N_mask,
  ZFlag     = Z_mask,
  CFlag     = C_mask,
  VFlag     = V_mask,
  NZFlag    = NFlag | ZFlag,
  NCFlag    = NFlag | CFlag,
  NVFlag    = NFlag | VFlag,
  ZCFlag    = ZFlag | CFlag,
  ZVFlag    = ZFlag | VFlag,
  CVFlag    = CFlag | VFlag,
  NZCFlag   = NFlag | ZFlag | CFlag,
  NZVFlag   = NFlag | ZFlag | VFlag,
  NCVFlag   = NFlag | CFlag | VFlag,
  ZCVFlag   = ZFlag | CFlag | VFlag,
  NZCVFlag  = NFlag | ZFlag | CFlag | VFlag,

  // Floating-point comparison results.
  FPEqualFlag       = ZCFlag,
  FPLessThanFlag    = NFlag,
  FPGreaterThanFlag = CFlag,
  FPUnorderedFlag   = CVFlag
};

enum Shift {
  NO_SHIFT = -1,
  LSL = 0x0,
  LSR = 0x1,
  ASR = 0x2,
  ROR = 0x3,
  MSL = 0x4
};

enum Extend {
  NO_EXTEND = -1,
  UXTB      = 0,
  UXTH      = 1,
  UXTW      = 2,
  UXTX      = 3,
  SXTB      = 4,
  SXTH      = 5,
  SXTW      = 6,
  SXTX      = 7
};

enum SystemHint {
  NOP   = 0,
  YIELD = 1,
  WFE   = 2,
  WFI   = 3,
  SEV   = 4,
  SEVL  = 5,
  ESB   = 16,
  CSDB  = 20
};

enum BarrierDomain {
  OuterShareable = 0,
  NonShareable   = 1,
  InnerShareable = 2,
  FullSystem     = 3
};

enum BarrierType {
  BarrierOther  = 0,
  BarrierReads  = 1,
  BarrierWrites = 2,
  BarrierAll    = 3
};

enum PrefetchOperation {
  PLDL1KEEP = 0x00,
  PLDL1STRM = 0x01,
  PLDL2KEEP = 0x02,
  PLDL2STRM = 0x03,
  PLDL3KEEP = 0x04,
  PLDL3STRM = 0x05,

  PLIL1KEEP = 0x08,
  PLIL1STRM = 0x09,
  PLIL2KEEP = 0x0a,
  PLIL2STRM = 0x0b,
  PLIL3KEEP = 0x0c,
  PLIL3STRM = 0x0d,

  PSTL1KEEP = 0x10,
  PSTL1STRM = 0x11,
  PSTL2KEEP = 0x12,
  PSTL2STRM = 0x13,
  PSTL3KEEP = 0x14,
  PSTL3STRM = 0x15
};

template<int op0, int op1, int crn, int crm, int op2>
class SystemRegisterEncoder {
 public:
  static const uint32_t value =
      ((op0 << SysO0_offset) |
       (op1 << SysOp1_offset) |
       (crn << CRn_offset) |
       (crm << CRm_offset) |
       (op2 << SysOp2_offset)) >> ImmSystemRegister_offset;
};

// System/special register names.
// This information is not encoded as one field but as the concatenation of
// multiple fields (Op0<0>, Op1, Crn, Crm, Op2).
enum SystemRegister {
  NZCV = SystemRegisterEncoder<3, 3, 4, 2, 0>::value,
  FPCR = SystemRegisterEncoder<3, 3, 4, 4, 0>::value
};

template<int op1, int crn, int crm, int op2>
class CacheOpEncoder {
 public:
  static const uint32_t value =
      ((op1 << SysOp1_offset) |
       (crn << CRn_offset) |
       (crm << CRm_offset) |
       (op2 << SysOp2_offset)) >> SysOp_offset;
};

enum InstructionCacheOp {
  IVAU = CacheOpEncoder<3, 7, 5, 1>::value
};

enum DataCacheOp {
  CVAC = CacheOpEncoder<3, 7, 10, 1>::value,
  CVAU = CacheOpEncoder<3, 7, 11, 1>::value,
  CIVAC = CacheOpEncoder<3, 7, 14, 1>::value,
  ZVA = CacheOpEncoder<3, 7, 4, 1>::value
};

// Instruction enumerations.
//
// These are the masks that define a class of instructions, and the list of
// instructions within each class. Each enumeration has a Fixed, FMask and
// Mask value.
//
// Fixed: The fixed bits in this instruction class.
// FMask: The mask used to extract the fixed bits in the class.
// Mask:  The mask used to identify the instructions within a class.
//
// The enumerations can be used like this:
//
// VIXL_ASSERT(instr->Mask(PCRelAddressingFMask) == PCRelAddressingFixed);
// switch(instr->Mask(PCRelAddressingMask)) {
//   case ADR:  Format("adr 'Xd, 'AddrPCRelByte"); break;
//   case ADRP: Format("adrp 'Xd, 'AddrPCRelPage"); break;
//   default:   printf("Unknown instruction\n");
// }


// Generic fields.
enum GenericInstrField : uint32_t {
  SixtyFourBits        = 0x80000000u,
  ThirtyTwoBits        = 0x00000000u,

  FPTypeMask           = 0x00C00000u,
  FP16                 = 0x00C00000u,
  FP32                 = 0x00000000u,
  FP64                 = 0x00400000u
};

enum NEONFormatField : uint32_t {
  NEONFormatFieldMask   = 0x40C00000u,
  NEON_Q                = 0x40000000u,
  NEON_8B               = 0x00000000u,
  NEON_16B              = NEON_8B | NEON_Q,
  NEON_4H               = 0x00400000u,
  NEON_8H               = NEON_4H | NEON_Q,
  NEON_2S               = 0x00800000u,
  NEON_4S               = NEON_2S | NEON_Q,
  NEON_1D               = 0x00C00000u,
  NEON_2D               = 0x00C00000u | NEON_Q
};

enum NEONFPFormatField : uint32_t {
  NEONFPFormatFieldMask = 0x40400000u,
  NEON_FP_4H            = FP16,
  NEON_FP_2S            = FP32,
  NEON_FP_8H            = FP16 | NEON_Q,
  NEON_FP_4S            = FP32 | NEON_Q,
  NEON_FP_2D            = FP64 | NEON_Q
};

enum NEONLSFormatField : uint32_t {
  NEONLSFormatFieldMask = 0x40000C00u,
  LS_NEON_8B            = 0x00000000u,
  LS_NEON_16B           = LS_NEON_8B | NEON_Q,
  LS_NEON_4H            = 0x00000400u,
  LS_NEON_8H            = LS_NEON_4H | NEON_Q,
  LS_NEON_2S            = 0x00000800u,
  LS_NEON_4S            = LS_NEON_2S | NEON_Q,
  LS_NEON_1D            = 0x00000C00u,
  LS_NEON_2D            = LS_NEON_1D | NEON_Q
};

enum NEONScalarFormatField : uint32_t {
  NEONScalarFormatFieldMask = 0x00C00000u,
  NEONScalar                = 0x10000000u,
  NEON_B                    = 0x00000000u,
  NEON_H                    = 0x00400000u,
  NEON_S                    = 0x00800000u,
  NEON_D                    = 0x00C00000u
};

// PC relative addressing.
enum PCRelAddressingOp : uint32_t {
  PCRelAddressingFixed = 0x10000000u,
  PCRelAddressingFMask = 0x1F000000u,
  PCRelAddressingMask  = 0x9F000000u,
  ADR                  = PCRelAddressingFixed | 0x00000000u,
  ADRP                 = PCRelAddressingFixed | 0x80000000u
};

// Add/sub (immediate, shifted and extended.)
const int kSFOffset = 31;
enum AddSubOp : uint32_t {
  AddSubOpMask      = 0x60000000u,
  AddSubSetFlagsBit = 0x20000000u,
  ADD               = 0x00000000u,
  ADDS              = ADD | AddSubSetFlagsBit,
  SUB               = 0x40000000u,
  SUBS              = SUB | AddSubSetFlagsBit
};

#define ADD_SUB_OP_LIST(V)  \
  V(ADD),                   \
  V(ADDS),                  \
  V(SUB),                   \
  V(SUBS)

enum AddSubImmediateOp : uint32_t {
  AddSubImmediateFixed = 0x11000000u,
  AddSubImmediateFMask = 0x1F000000u,
  AddSubImmediateMask  = 0xFF000000u,
  #define ADD_SUB_IMMEDIATE(A)           \
  A##_w_imm = AddSubImmediateFixed | A,  \
  A##_x_imm = AddSubImmediateFixed | A | SixtyFourBits
  ADD_SUB_OP_LIST(ADD_SUB_IMMEDIATE)
  #undef ADD_SUB_IMMEDIATE
};

enum AddSubShiftedOp : uint32_t {
  AddSubShiftedFixed   = 0x0B000000u,
  AddSubShiftedFMask   = 0x1F200000u,
  AddSubShiftedMask    = 0xFF200000u,
  #define ADD_SUB_SHIFTED(A)             \
  A##_w_shift = AddSubShiftedFixed | A,  \
  A##_x_shift = AddSubShiftedFixed | A | SixtyFourBits
  ADD_SUB_OP_LIST(ADD_SUB_SHIFTED)
  #undef ADD_SUB_SHIFTED
};

enum AddSubExtendedOp : uint32_t {
  AddSubExtendedFixed  = 0x0B200000u,
  AddSubExtendedFMask  = 0x1F200000u,
  AddSubExtendedMask   = 0xFFE00000u,
  #define ADD_SUB_EXTENDED(A)           \
  A##_w_ext = AddSubExtendedFixed | A,  \
  A##_x_ext = AddSubExtendedFixed | A | SixtyFourBits
  ADD_SUB_OP_LIST(ADD_SUB_EXTENDED)
  #undef ADD_SUB_EXTENDED
};

// Add/sub with carry.
enum AddSubWithCarryOp : uint32_t {
  AddSubWithCarryFixed = 0x1A000000u,
  AddSubWithCarryFMask = 0x1FE00000u,
  AddSubWithCarryMask  = 0xFFE0FC00u,
  ADC_w                = AddSubWithCarryFixed | ADD,
  ADC_x                = AddSubWithCarryFixed | ADD | SixtyFourBits,
  ADC                  = ADC_w,
  ADCS_w               = AddSubWithCarryFixed | ADDS,
  ADCS_x               = AddSubWithCarryFixed | ADDS | SixtyFourBits,
  SBC_w                = AddSubWithCarryFixed | SUB,
  SBC_x                = AddSubWithCarryFixed | SUB | SixtyFourBits,
  SBC                  = SBC_w,
  SBCS_w               = AddSubWithCarryFixed | SUBS,
  SBCS_x               = AddSubWithCarryFixed | SUBS | SixtyFourBits
};


// Logical (immediate and shifted register).
enum LogicalOp : uint32_t {
  LogicalOpMask = 0x60200000u,
  NOT   = 0x00200000u,
  AND   = 0x00000000u,
  BIC   = AND | NOT,
  ORR   = 0x20000000u,
  ORN   = ORR | NOT,
  EOR   = 0x40000000u,
  EON   = EOR | NOT,
  ANDS  = 0x60000000u,
  BICS  = ANDS | NOT
};

// Logical immediate.
enum LogicalImmediateOp : uint32_t {
  LogicalImmediateFixed = 0x12000000u,
  LogicalImmediateFMask = 0x1F800000u,
  LogicalImmediateMask  = 0xFF800000u,
  AND_w_imm   = LogicalImmediateFixed | AND,
  AND_x_imm   = LogicalImmediateFixed | AND | SixtyFourBits,
  ORR_w_imm   = LogicalImmediateFixed | ORR,
  ORR_x_imm   = LogicalImmediateFixed | ORR | SixtyFourBits,
  EOR_w_imm   = LogicalImmediateFixed | EOR,
  EOR_x_imm   = LogicalImmediateFixed | EOR | SixtyFourBits,
  ANDS_w_imm  = LogicalImmediateFixed | ANDS,
  ANDS_x_imm  = LogicalImmediateFixed | ANDS | SixtyFourBits
};

// Logical shifted register.
enum LogicalShiftedOp : uint32_t {
  LogicalShiftedFixed = 0x0A000000u,
  LogicalShiftedFMask = 0x1F000000u,
  LogicalShiftedMask  = 0xFF200000u,
  AND_w               = LogicalShiftedFixed | AND,
  AND_x               = LogicalShiftedFixed | AND | SixtyFourBits,
  AND_shift           = AND_w,
  BIC_w               = LogicalShiftedFixed | BIC,
  BIC_x               = LogicalShiftedFixed | BIC | SixtyFourBits,
  BIC_shift           = BIC_w,
  ORR_w               = LogicalShiftedFixed | ORR,
  ORR_x               = LogicalShiftedFixed | ORR | SixtyFourBits,
  ORR_shift           = ORR_w,
  ORN_w               = LogicalShiftedFixed | ORN,
  ORN_x               = LogicalShiftedFixed | ORN | SixtyFourBits,
  ORN_shift           = ORN_w,
  EOR_w               = LogicalShiftedFixed | EOR,
  EOR_x               = LogicalShiftedFixed | EOR | SixtyFourBits,
  EOR_shift           = EOR_w,
  EON_w               = LogicalShiftedFixed | EON,
  EON_x               = LogicalShiftedFixed | EON | SixtyFourBits,
  EON_shift           = EON_w,
  ANDS_w              = LogicalShiftedFixed | ANDS,
  ANDS_x              = LogicalShiftedFixed | ANDS | SixtyFourBits,
  ANDS_shift          = ANDS_w,
  BICS_w              = LogicalShiftedFixed | BICS,
  BICS_x              = LogicalShiftedFixed | BICS | SixtyFourBits,
  BICS_shift          = BICS_w
};

// Move wide immediate.
enum MoveWideImmediateOp : uint32_t {
  MoveWideImmediateFixed = 0x12800000u,
  MoveWideImmediateFMask = 0x1F800000u,
  MoveWideImmediateMask  = 0xFF800000u,
  MOVN                   = 0x00000000u,
  MOVZ                   = 0x40000000u,
  MOVK                   = 0x60000000u,
  MOVN_w                 = MoveWideImmediateFixed | MOVN,
  MOVN_x                 = MoveWideImmediateFixed | MOVN | SixtyFourBits,
  MOVZ_w                 = MoveWideImmediateFixed | MOVZ,
  MOVZ_x                 = MoveWideImmediateFixed | MOVZ | SixtyFourBits,
  MOVK_w                 = MoveWideImmediateFixed | MOVK,
  MOVK_x                 = MoveWideImmediateFixed | MOVK | SixtyFourBits
};

// Bitfield.
const int kBitfieldNOffset = 22;
enum BitfieldOp : uint32_t {
  BitfieldFixed = 0x13000000u,
  BitfieldFMask = 0x1F800000u,
  BitfieldMask  = 0xFF800000u,
  SBFM_w        = BitfieldFixed | 0x00000000u,
  SBFM_x        = BitfieldFixed | 0x80000000u,
  SBFM          = SBFM_w,
  BFM_w         = BitfieldFixed | 0x20000000u,
  BFM_x         = BitfieldFixed | 0xA0000000u,
  BFM           = BFM_w,
  UBFM_w        = BitfieldFixed | 0x40000000u,
  UBFM_x        = BitfieldFixed | 0xC0000000u,
  UBFM          = UBFM_w
  // Bitfield N field.
};

// Extract.
enum ExtractOp : uint32_t {
  ExtractFixed = 0x13800000u,
  ExtractFMask = 0x1F800000u,
  ExtractMask  = 0xFFA00000u,
  EXTR_w       = ExtractFixed | 0x00000000u,
  EXTR_x       = ExtractFixed | 0x80000000u,
  EXTR         = EXTR_w
};

// Unconditional branch.
enum UnconditionalBranchOp : uint32_t {
  UnconditionalBranchFixed = 0x14000000u,
  UnconditionalBranchFMask = 0x7C000000u,
  UnconditionalBranchMask  = 0xFC000000u,
  B                        = UnconditionalBranchFixed | 0x00000000u,
  BL                       = UnconditionalBranchFixed | 0x80000000u
};

// Unconditional branch to register.
enum UnconditionalBranchToRegisterOp : uint32_t {
  UnconditionalBranchToRegisterFixed = 0xD6000000u,
  UnconditionalBranchToRegisterFMask = 0xFE000000u,
  UnconditionalBranchToRegisterMask  = 0xFFFFFC00u,
  BR      = UnconditionalBranchToRegisterFixed | 0x001F0000u,
  BLR     = UnconditionalBranchToRegisterFixed | 0x003F0000u,
  RET     = UnconditionalBranchToRegisterFixed | 0x005F0000u,

  BRAAZ  = UnconditionalBranchToRegisterFixed | 0x001F0800u,
  BRABZ  = UnconditionalBranchToRegisterFixed | 0x001F0C00u,
  BLRAAZ = UnconditionalBranchToRegisterFixed | 0x003F0800u,
  BLRABZ = UnconditionalBranchToRegisterFixed | 0x003F0C00u,
  RETAA  = UnconditionalBranchToRegisterFixed | 0x005F0800u,
  RETAB  = UnconditionalBranchToRegisterFixed | 0x005F0C00u,
  BRAA   = UnconditionalBranchToRegisterFixed | 0x011F0800u,
  BRAB   = UnconditionalBranchToRegisterFixed | 0x011F0C00u,
  BLRAA  = UnconditionalBranchToRegisterFixed | 0x013F0800u,
  BLRAB  = UnconditionalBranchToRegisterFixed | 0x013F0C00u
};

// Compare and branch.
enum CompareBranchOp : uint32_t {
  CompareBranchFixed = 0x34000000u,
  CompareBranchFMask = 0x7E000000u,
  CompareBranchMask  = 0xFF000000u,
  CBZ_w              = CompareBranchFixed | 0x00000000u,
  CBZ_x              = CompareBranchFixed | 0x80000000u,
  CBZ                = CBZ_w,
  CBNZ_w             = CompareBranchFixed | 0x01000000u,
  CBNZ_x             = CompareBranchFixed | 0x81000000u,
  CBNZ               = CBNZ_w
};

// Test and branch.
enum TestBranchOp : uint32_t {
  TestBranchFixed = 0x36000000u,
  TestBranchFMask = 0x7E000000u,
  TestBranchMask  = 0x7F000000u,
  TBZ             = TestBranchFixed | 0x00000000u,
  TBNZ            = TestBranchFixed | 0x01000000u
};

// Conditional branch.
enum ConditionalBranchOp : uint32_t {
  ConditionalBranchFixed = 0x54000000u,
  ConditionalBranchFMask = 0xFE000000u,
  ConditionalBranchMask  = 0xFF000010u,
  B_cond                 = ConditionalBranchFixed | 0x00000000u
};

// System.
// System instruction encoding is complicated because some instructions use op
// and CR fields to encode parameters. To handle this cleanly, the system
// instructions are split into more than one enum.

enum SystemOp : uint32_t {
  SystemFixed = 0xD5000000u,
  SystemFMask = 0xFFC00000u
};

enum SystemSysRegOp : uint32_t {
  SystemSysRegFixed = 0xD5100000u,
  SystemSysRegFMask = 0xFFD00000u,
  SystemSysRegMask  = 0xFFF00000u,
  MRS               = SystemSysRegFixed | 0x00200000u,
  MSR               = SystemSysRegFixed | 0x00000000u
};

enum SystemHintOp : uint32_t {
  SystemHintFixed = 0xD503201Fu,
  SystemHintFMask = 0xFFFFF01Fu,
  SystemHintMask  = 0xFFFFF01Fu,
  HINT            = SystemHintFixed | 0x00000000u
};

enum SystemSysOp : uint32_t {
  SystemSysFixed  = 0xD5080000u,
  SystemSysFMask  = 0xFFF80000u,
  SystemSysMask   = 0xFFF80000u,
  SYS             = SystemSysFixed | 0x00000000u
};

// Exception.
enum ExceptionOp : uint32_t {
  ExceptionFixed = 0xD4000000u,
  ExceptionFMask = 0xFF000000u,
  ExceptionMask  = 0xFFE0001Fu,
  HLT            = ExceptionFixed | 0x00400000u,
  BRK            = ExceptionFixed | 0x00200000u,
  SVC            = ExceptionFixed | 0x00000001u,
  HVC            = ExceptionFixed | 0x00000002u,
  SMC            = ExceptionFixed | 0x00000003u,
  DCPS1          = ExceptionFixed | 0x00A00001u,
  DCPS2          = ExceptionFixed | 0x00A00002u,
  DCPS3          = ExceptionFixed | 0x00A00003u
};

enum MemBarrierOp : uint32_t {
  MemBarrierFixed = 0xD503309Fu,
  MemBarrierFMask = 0xFFFFF09Fu,
  MemBarrierMask  = 0xFFFFF0FFu,
  DSB             = MemBarrierFixed | 0x00000000u,
  DMB             = MemBarrierFixed | 0x00000020u,
  ISB             = MemBarrierFixed | 0x00000040u
};

enum SystemExclusiveMonitorOp : uint32_t {
  SystemExclusiveMonitorFixed = 0xD503305Fu,
  SystemExclusiveMonitorFMask = 0xFFFFF0FFu,
  SystemExclusiveMonitorMask  = 0xFFFFF0FFu,
  CLREX                       = SystemExclusiveMonitorFixed
};

enum SystemPAuthOp : uint32_t {
  SystemPAuthFixed = 0xD503211Fu,
  SystemPAuthFMask = 0xFFFFFD1Fu,
  SystemPAuthMask  = 0xFFFFFFFFu,
  PACIA1716 = SystemPAuthFixed | 0x00000100u,
  PACIB1716 = SystemPAuthFixed | 0x00000140u,
  AUTIA1716 = SystemPAuthFixed | 0x00000180u,
  AUTIB1716 = SystemPAuthFixed | 0x000001C0u,
  PACIAZ    = SystemPAuthFixed | 0x00000300u,
  PACIASP   = SystemPAuthFixed | 0x00000320u,
  PACIBZ    = SystemPAuthFixed | 0x00000340u,
  PACIBSP   = SystemPAuthFixed | 0x00000360u,
  AUTIAZ    = SystemPAuthFixed | 0x00000380u,
  AUTIASP   = SystemPAuthFixed | 0x000003A0u,
  AUTIBZ    = SystemPAuthFixed | 0x000003C0u,
  AUTIBSP   = SystemPAuthFixed | 0x000003E0u,

  // XPACLRI has the same fixed mask as System Hints and needs to be handled
  // differently.
  XPACLRI   = 0xD50320FFu
};

// Any load or store.
enum LoadStoreAnyOp : uint32_t {
  LoadStoreAnyFMask = 0x0a000000u,
  LoadStoreAnyFixed = 0x08000000u
};

// Any load pair or store pair.
enum LoadStorePairAnyOp : uint32_t {
  LoadStorePairAnyFMask = 0x3a000000u,
  LoadStorePairAnyFixed = 0x28000000u
};

#define LOAD_STORE_PAIR_OP_LIST(V)  \
  V(STP, w,   0x00000000u),          \
  V(LDP, w,   0x00400000u),          \
  V(LDPSW, x, 0x40400000u),          \
  V(STP, x,   0x80000000u),          \
  V(LDP, x,   0x80400000u),          \
  V(STP, s,   0x04000000u),          \
  V(LDP, s,   0x04400000u),          \
  V(STP, d,   0x44000000u),          \
  V(LDP, d,   0x44400000u),          \
  V(STP, q,   0x84000000u),          \
  V(LDP, q,   0x84400000u)

// Load/store pair (post, pre and offset.)
enum LoadStorePairOp : uint32_t {
  LoadStorePairMask = 0xC4400000u,
  LoadStorePairLBit = 1 << 22,
  #define LOAD_STORE_PAIR(A, B, C) \
  A##_##B = C
  LOAD_STORE_PAIR_OP_LIST(LOAD_STORE_PAIR)
  #undef LOAD_STORE_PAIR
};

enum LoadStorePairPostIndexOp : uint32_t {
  LoadStorePairPostIndexFixed = 0x28800000u,
  LoadStorePairPostIndexFMask = 0x3B800000u,
  LoadStorePairPostIndexMask  = 0xFFC00000u,
  #define LOAD_STORE_PAIR_POST_INDEX(A, B, C)  \
  A##_##B##_post = LoadStorePairPostIndexFixed | A##_##B
  LOAD_STORE_PAIR_OP_LIST(LOAD_STORE_PAIR_POST_INDEX)
  #undef LOAD_STORE_PAIR_POST_INDEX
};

enum LoadStorePairPreIndexOp : uint32_t {
  LoadStorePairPreIndexFixed = 0x29800000u,
  LoadStorePairPreIndexFMask = 0x3B800000u,
  LoadStorePairPreIndexMask  = 0xFFC00000u,
  #define LOAD_STORE_PAIR_PRE_INDEX(A, B, C)  \
  A##_##B##_pre = LoadStorePairPreIndexFixed | A##_##B
  LOAD_STORE_PAIR_OP_LIST(LOAD_STORE_PAIR_PRE_INDEX)
  #undef LOAD_STORE_PAIR_PRE_INDEX
};

enum LoadStorePairOffsetOp : uint32_t {
  LoadStorePairOffsetFixed = 0x29000000u,
  LoadStorePairOffsetFMask = 0x3B800000u,
  LoadStorePairOffsetMask  = 0xFFC00000u,
  #define LOAD_STORE_PAIR_OFFSET(A, B, C)  \
  A##_##B##_off = LoadStorePairOffsetFixed | A##_##B
  LOAD_STORE_PAIR_OP_LIST(LOAD_STORE_PAIR_OFFSET)
  #undef LOAD_STORE_PAIR_OFFSET
};

enum LoadStorePairNonTemporalOp : uint32_t {
  LoadStorePairNonTemporalFixed = 0x28000000u,
  LoadStorePairNonTemporalFMask = 0x3B800000u,
  LoadStorePairNonTemporalMask  = 0xFFC00000u,
  LoadStorePairNonTemporalLBit = 1 << 22,
  STNP_w = LoadStorePairNonTemporalFixed | STP_w,
  LDNP_w = LoadStorePairNonTemporalFixed | LDP_w,
  STNP_x = LoadStorePairNonTemporalFixed | STP_x,
  LDNP_x = LoadStorePairNonTemporalFixed | LDP_x,
  STNP_s = LoadStorePairNonTemporalFixed | STP_s,
  LDNP_s = LoadStorePairNonTemporalFixed | LDP_s,
  STNP_d = LoadStorePairNonTemporalFixed | STP_d,
  LDNP_d = LoadStorePairNonTemporalFixed | LDP_d,
  STNP_q = LoadStorePairNonTemporalFixed | STP_q,
  LDNP_q = LoadStorePairNonTemporalFixed | LDP_q
};

// Load literal.
enum LoadLiteralOp : uint32_t {
  LoadLiteralFixed = 0x18000000u,
  LoadLiteralFMask = 0x3B000000u,
  LoadLiteralMask  = 0xFF000000u,
  LDR_w_lit        = LoadLiteralFixed | 0x00000000u,
  LDR_x_lit        = LoadLiteralFixed | 0x40000000u,
  LDRSW_x_lit      = LoadLiteralFixed | 0x80000000u,
  PRFM_lit         = LoadLiteralFixed | 0xC0000000u,
  LDR_s_lit        = LoadLiteralFixed | 0x04000000u,
  LDR_d_lit        = LoadLiteralFixed | 0x44000000u,
  LDR_q_lit        = LoadLiteralFixed | 0x84000000u
};

#define LOAD_STORE_OP_LIST(V)     \
  V(ST, RB, w,  0x00000000u),  \
  V(ST, RH, w,  0x40000000u),  \
  V(ST, R, w,   0x80000000u),  \
  V(ST, R, x,   0xC0000000u),  \
  V(LD, RB, w,  0x00400000u),  \
  V(LD, RH, w,  0x40400000u),  \
  V(LD, R, w,   0x80400000u),  \
  V(LD, R, x,   0xC0400000u),  \
  V(LD, RSB, x, 0x00800000u),  \
  V(LD, RSH, x, 0x40800000u),  \
  V(LD, RSW, x, 0x80800000u),  \
  V(LD, RSB, w, 0x00C00000u),  \
  V(LD, RSH, w, 0x40C00000u),  \
  V(ST, R, b,   0x04000000u),  \
  V(ST, R, h,   0x44000000u),  \
  V(ST, R, s,   0x84000000u),  \
  V(ST, R, d,   0xC4000000u),  \
  V(ST, R, q,   0x04800000u),  \
  V(LD, R, b,   0x04400000u),  \
  V(LD, R, h,   0x44400000u),  \
  V(LD, R, s,   0x84400000u),  \
  V(LD, R, d,   0xC4400000u),  \
  V(LD, R, q,   0x04C00000u)

// Load/store (post, pre, offset and unsigned.)
enum LoadStoreOp : uint32_t {
  LoadStoreMask = 0xC4C00000u,
  LoadStoreVMask = 0x04000000u,
  #define LOAD_STORE(A, B, C, D)  \
  A##B##_##C = D
  LOAD_STORE_OP_LIST(LOAD_STORE),
  #undef LOAD_STORE
  PRFM = 0xC0800000u
};

// Load/store unscaled offset.
enum LoadStoreUnscaledOffsetOp : uint32_t {
  LoadStoreUnscaledOffsetFixed = 0x38000000u,
  LoadStoreUnscaledOffsetFMask = 0x3B200C00u,
  LoadStoreUnscaledOffsetMask  = 0xFFE00C00u,
  PRFUM                        = LoadStoreUnscaledOffsetFixed | PRFM,
  #define LOAD_STORE_UNSCALED(A, B, C, D)  \
  A##U##B##_##C = LoadStoreUnscaledOffsetFixed | D
  LOAD_STORE_OP_LIST(LOAD_STORE_UNSCALED)
  #undef LOAD_STORE_UNSCALED
};

// Load/store post index.
enum LoadStorePostIndex : uint32_t {
  LoadStorePostIndexFixed = 0x38000400u,
  LoadStorePostIndexFMask = 0x3B200C00u,
  LoadStorePostIndexMask  = 0xFFE00C00u,
  #define LOAD_STORE_POST_INDEX(A, B, C, D)  \
  A##B##_##C##_post = LoadStorePostIndexFixed | D
  LOAD_STORE_OP_LIST(LOAD_STORE_POST_INDEX)
  #undef LOAD_STORE_POST_INDEX
};

// Load/store pre index.
enum LoadStorePreIndex : uint32_t {
  LoadStorePreIndexFixed = 0x38000C00u,
  LoadStorePreIndexFMask = 0x3B200C00u,
  LoadStorePreIndexMask  = 0xFFE00C00u,
  #define LOAD_STORE_PRE_INDEX(A, B, C, D)  \
  A##B##_##C##_pre = LoadStorePreIndexFixed | D
  LOAD_STORE_OP_LIST(LOAD_STORE_PRE_INDEX)
  #undef LOAD_STORE_PRE_INDEX
};

// Load/store unsigned offset.
enum LoadStoreUnsignedOffset : uint32_t {
  LoadStoreUnsignedOffsetFixed = 0x39000000u,
  LoadStoreUnsignedOffsetFMask = 0x3B000000u,
  LoadStoreUnsignedOffsetMask  = 0xFFC00000u,
  PRFM_unsigned                = LoadStoreUnsignedOffsetFixed | PRFM,
  #define LOAD_STORE_UNSIGNED_OFFSET(A, B, C, D) \
  A##B##_##C##_unsigned = LoadStoreUnsignedOffsetFixed | D
  LOAD_STORE_OP_LIST(LOAD_STORE_UNSIGNED_OFFSET)
  #undef LOAD_STORE_UNSIGNED_OFFSET
};

// Load/store register offset.
enum LoadStoreRegisterOffset : uint32_t {
  LoadStoreRegisterOffsetFixed = 0x38200800u,
  LoadStoreRegisterOffsetFMask = 0x3B200C00u,
  LoadStoreRegisterOffsetMask  = 0xFFE00C00u,
  PRFM_reg                     = LoadStoreRegisterOffsetFixed | PRFM,
  #define LOAD_STORE_REGISTER_OFFSET(A, B, C, D) \
  A##B##_##C##_reg = LoadStoreRegisterOffsetFixed | D
  LOAD_STORE_OP_LIST(LOAD_STORE_REGISTER_OFFSET)
  #undef LOAD_STORE_REGISTER_OFFSET
};

enum LoadStoreExclusive : uint32_t {
  LoadStoreExclusiveFixed = 0x08000000u,
  LoadStoreExclusiveFMask = 0x3F000000u,
  LoadStoreExclusiveMask  = 0xFFE08000u,
  STXRB_w  = LoadStoreExclusiveFixed | 0x00000000u,
  STXRH_w  = LoadStoreExclusiveFixed | 0x40000000u,
  STXR_w   = LoadStoreExclusiveFixed | 0x80000000u,
  STXR_x   = LoadStoreExclusiveFixed | 0xC0000000u,
  LDXRB_w  = LoadStoreExclusiveFixed | 0x00400000u,
  LDXRH_w  = LoadStoreExclusiveFixed | 0x40400000u,
  LDXR_w   = LoadStoreExclusiveFixed | 0x80400000u,
  LDXR_x   = LoadStoreExclusiveFixed | 0xC0400000u,
  STXP_w   = LoadStoreExclusiveFixed | 0x80200000u,
  STXP_x   = LoadStoreExclusiveFixed | 0xC0200000u,
  LDXP_w   = LoadStoreExclusiveFixed | 0x80600000u,
  LDXP_x   = LoadStoreExclusiveFixed | 0xC0600000u,
  STLXRB_w = LoadStoreExclusiveFixed | 0x00008000u,
  STLXRH_w = LoadStoreExclusiveFixed | 0x40008000u,
  STLXR_w  = LoadStoreExclusiveFixed | 0x80008000u,
  STLXR_x  = LoadStoreExclusiveFixed | 0xC0008000u,
  LDAXRB_w = LoadStoreExclusiveFixed | 0x00408000u,
  LDAXRH_w = LoadStoreExclusiveFixed | 0x40408000u,
  LDAXR_w  = LoadStoreExclusiveFixed | 0x80408000u,
  LDAXR_x  = LoadStoreExclusiveFixed | 0xC0408000u,
  STLXP_w  = LoadStoreExclusiveFixed | 0x80208000u,
  STLXP_x  = LoadStoreExclusiveFixed | 0xC0208000u,
  LDAXP_w  = LoadStoreExclusiveFixed | 0x80608000u,
  LDAXP_x  = LoadStoreExclusiveFixed | 0xC0608000u,
  STLRB_w  = LoadStoreExclusiveFixed | 0x00808000u,
  STLRH_w  = LoadStoreExclusiveFixed | 0x40808000u,
  STLR_w   = LoadStoreExclusiveFixed | 0x80808000u,
  STLR_x   = LoadStoreExclusiveFixed | 0xC0808000u,
  LDARB_w  = LoadStoreExclusiveFixed | 0x00C08000u,
  LDARH_w  = LoadStoreExclusiveFixed | 0x40C08000u,
  LDAR_w   = LoadStoreExclusiveFixed | 0x80C08000u,
  LDAR_x   = LoadStoreExclusiveFixed | 0xC0C08000u,

  // v8.1 Load/store LORegion ops
  STLLRB   = LoadStoreExclusiveFixed | 0x00800000u,
  LDLARB   = LoadStoreExclusiveFixed | 0x00C00000u,
  STLLRH   = LoadStoreExclusiveFixed | 0x40800000u,
  LDLARH   = LoadStoreExclusiveFixed | 0x40C00000u,
  STLLR_w  = LoadStoreExclusiveFixed | 0x80800000u,
  LDLAR_w  = LoadStoreExclusiveFixed | 0x80C00000u,
  STLLR_x  = LoadStoreExclusiveFixed | 0xC0800000u,
  LDLAR_x  = LoadStoreExclusiveFixed | 0xC0C00000u,

  // v8.1 Load/store exclusive ops
  LSEBit_l  = 0x00400000u,
  LSEBit_o0 = 0x00008000u,
  LSEBit_sz = 0x40000000u,
  CASFixed  = LoadStoreExclusiveFixed | 0x80A00000u,
  CASBFixed = LoadStoreExclusiveFixed | 0x00A00000u,
  CASHFixed = LoadStoreExclusiveFixed | 0x40A00000u,
  CASPFixed = LoadStoreExclusiveFixed | 0x00200000u,
  CAS_w    = CASFixed,
  CAS_x    = CASFixed | LSEBit_sz,
  CASA_w   = CASFixed | LSEBit_l,
  CASA_x   = CASFixed | LSEBit_l | LSEBit_sz,
  CASL_w   = CASFixed | LSEBit_o0,
  CASL_x   = CASFixed | LSEBit_o0 | LSEBit_sz,
  CASAL_w  = CASFixed | LSEBit_l | LSEBit_o0,
  CASAL_x  = CASFixed | LSEBit_l | LSEBit_o0 | LSEBit_sz,
  CASB     = CASBFixed,
  CASAB    = CASBFixed | LSEBit_l,
  CASLB    = CASBFixed | LSEBit_o0,
  CASALB   = CASBFixed | LSEBit_l | LSEBit_o0,
  CASH     = CASHFixed,
  CASAH    = CASHFixed | LSEBit_l,
  CASLH    = CASHFixed | LSEBit_o0,
  CASALH   = CASHFixed | LSEBit_l | LSEBit_o0,
  CASP_w   = CASPFixed,
  CASP_x   = CASPFixed | LSEBit_sz,
  CASPA_w  = CASPFixed | LSEBit_l,
  CASPA_x  = CASPFixed | LSEBit_l | LSEBit_sz,
  CASPL_w  = CASPFixed | LSEBit_o0,
  CASPL_x  = CASPFixed | LSEBit_o0 | LSEBit_sz,
  CASPAL_w = CASPFixed | LSEBit_l | LSEBit_o0,
  CASPAL_x = CASPFixed | LSEBit_l | LSEBit_o0 | LSEBit_sz
};

#define ATOMIC_MEMORY_SIMPLE_OPC_LIST(V) \
  V(LDADD, 0x00000000u),                  \
  V(LDCLR, 0x00001000u),                  \
  V(LDEOR, 0x00002000u),                  \
  V(LDSET, 0x00003000u),                  \
  V(LDSMAX, 0x00004000u),                 \
  V(LDSMIN, 0x00005000u),                 \
  V(LDUMAX, 0x00006000u),                 \
  V(LDUMIN, 0x00007000u)

// Atomic memory.
enum AtomicMemoryOp : uint32_t {
  AtomicMemoryFixed = 0x38200000u,
  AtomicMemoryFMask = 0x3B200C00u,
  AtomicMemoryMask = 0xFFE0FC00u,
  SWPB = AtomicMemoryFixed | 0x00008000u,
  SWPAB = AtomicMemoryFixed | 0x00808000u,
  SWPLB = AtomicMemoryFixed | 0x00408000u,
  SWPALB = AtomicMemoryFixed | 0x00C08000u,
  SWPH = AtomicMemoryFixed | 0x40008000u,
  SWPAH = AtomicMemoryFixed | 0x40808000u,
  SWPLH = AtomicMemoryFixed | 0x40408000u,
  SWPALH = AtomicMemoryFixed | 0x40C08000u,
  SWP_w = AtomicMemoryFixed | 0x80008000u,
  SWPA_w = AtomicMemoryFixed | 0x80808000u,
  SWPL_w = AtomicMemoryFixed | 0x80408000u,
  SWPAL_w = AtomicMemoryFixed | 0x80C08000u,
  SWP_x = AtomicMemoryFixed | 0xC0008000u,
  SWPA_x = AtomicMemoryFixed | 0xC0808000u,
  SWPL_x = AtomicMemoryFixed | 0xC0408000u,
  SWPAL_x = AtomicMemoryFixed | 0xC0C08000u,
  LDAPRB = AtomicMemoryFixed | 0x0080C000u,
  LDAPRH = AtomicMemoryFixed | 0x4080C000u,
  LDAPR_w = AtomicMemoryFixed | 0x8080C000u,
  LDAPR_x = AtomicMemoryFixed | 0xC080C000u,

  AtomicMemorySimpleFMask = 0x3B208C00u,
  AtomicMemorySimpleOpMask = 0x00007000u,
#define ATOMIC_MEMORY_SIMPLE(N, OP)              \
  N##Op = OP,                                    \
  N##B = AtomicMemoryFixed | OP,                 \
  N##AB = AtomicMemoryFixed | OP | 0x00800000u,   \
  N##LB = AtomicMemoryFixed | OP | 0x00400000u,   \
  N##ALB = AtomicMemoryFixed | OP | 0x00C00000u,  \
  N##H = AtomicMemoryFixed | OP | 0x40000000u,    \
  N##AH = AtomicMemoryFixed | OP | 0x40800000u,   \
  N##LH = AtomicMemoryFixed | OP | 0x40400000u,   \
  N##ALH = AtomicMemoryFixed | OP | 0x40C00000u,  \
  N##_w = AtomicMemoryFixed | OP | 0x80000000u,   \
  N##A_w = AtomicMemoryFixed | OP | 0x80800000u,  \
  N##L_w = AtomicMemoryFixed | OP | 0x80400000u,  \
  N##AL_w = AtomicMemoryFixed | OP | 0x80C00000u, \
  N##_x = AtomicMemoryFixed | OP | 0xC0000000u,   \
  N##A_x = AtomicMemoryFixed | OP | 0xC0800000u,  \
  N##L_x = AtomicMemoryFixed | OP | 0xC0400000u,  \
  N##AL_x = AtomicMemoryFixed | OP | 0xC0C00000u

  ATOMIC_MEMORY_SIMPLE_OPC_LIST(ATOMIC_MEMORY_SIMPLE)
#undef ATOMIC_MEMORY_SIMPLE
};

// Conditional compare.
enum ConditionalCompareOp : uint32_t {
  ConditionalCompareMask = 0x60000000u,
  CCMN                   = 0x20000000u,
  CCMP                   = 0x60000000u
};

// Conditional compare register.
enum ConditionalCompareRegisterOp : uint32_t {
  ConditionalCompareRegisterFixed = 0x1A400000u,
  ConditionalCompareRegisterFMask = 0x1FE00800u,
  ConditionalCompareRegisterMask  = 0xFFE00C10u,
  CCMN_w = ConditionalCompareRegisterFixed | CCMN,
  CCMN_x = ConditionalCompareRegisterFixed | SixtyFourBits | CCMN,
  CCMP_w = ConditionalCompareRegisterFixed | CCMP,
  CCMP_x = ConditionalCompareRegisterFixed | SixtyFourBits | CCMP
};

// Conditional compare immediate.
enum ConditionalCompareImmediateOp : uint32_t {
  ConditionalCompareImmediateFixed = 0x1A400800u,
  ConditionalCompareImmediateFMask = 0x1FE00800u,
  ConditionalCompareImmediateMask  = 0xFFE00C10u,
  CCMN_w_imm = ConditionalCompareImmediateFixed | CCMN,
  CCMN_x_imm = ConditionalCompareImmediateFixed | SixtyFourBits | CCMN,
  CCMP_w_imm = ConditionalCompareImmediateFixed | CCMP,
  CCMP_x_imm = ConditionalCompareImmediateFixed | SixtyFourBits | CCMP
};

// Conditional select.
enum ConditionalSelectOp : uint32_t {
  ConditionalSelectFixed = 0x1A800000u,
  ConditionalSelectFMask = 0x1FE00000u,
  ConditionalSelectMask  = 0xFFE00C00u,
  CSEL_w                 = ConditionalSelectFixed | 0x00000000u,
  CSEL_x                 = ConditionalSelectFixed | 0x80000000u,
  CSEL                   = CSEL_w,
  CSINC_w                = ConditionalSelectFixed | 0x00000400u,
  CSINC_x                = ConditionalSelectFixed | 0x80000400u,
  CSINC                  = CSINC_w,
  CSINV_w                = ConditionalSelectFixed | 0x40000000u,
  CSINV_x                = ConditionalSelectFixed | 0xC0000000u,
  CSINV                  = CSINV_w,
  CSNEG_w                = ConditionalSelectFixed | 0x40000400u,
  CSNEG_x                = ConditionalSelectFixed | 0xC0000400u,
  CSNEG                  = CSNEG_w
};

// Data processing 1 source.
enum DataProcessing1SourceOp : uint32_t {
  DataProcessing1SourceFixed = 0x5AC00000u,
  DataProcessing1SourceFMask = 0x5FE00000u,
  DataProcessing1SourceMask  = 0xFFFFFC00u,
  RBIT    = DataProcessing1SourceFixed | 0x00000000u,
  RBIT_w  = RBIT,
  RBIT_x  = RBIT | SixtyFourBits,
  REV16   = DataProcessing1SourceFixed | 0x00000400u,
  REV16_w = REV16,
  REV16_x = REV16 | SixtyFourBits,
  REV     = DataProcessing1SourceFixed | 0x00000800u,
  REV_w   = REV,
  REV32_x = REV | SixtyFourBits,
  REV_x   = DataProcessing1SourceFixed | SixtyFourBits | 0x00000C00u,
  CLZ     = DataProcessing1SourceFixed | 0x00001000u,
  CLZ_w   = CLZ,
  CLZ_x   = CLZ | SixtyFourBits,
  CLS     = DataProcessing1SourceFixed | 0x00001400u,
  CLS_w   = CLS,
  CLS_x   = CLS | SixtyFourBits,

  // Pointer authentication instructions in Armv8.3.
  PACIA  = DataProcessing1SourceFixed | 0x80010000u,
  PACIB  = DataProcessing1SourceFixed | 0x80010400u,
  PACDA  = DataProcessing1SourceFixed | 0x80010800u,
  PACDB  = DataProcessing1SourceFixed | 0x80010C00u,
  AUTIA  = DataProcessing1SourceFixed | 0x80011000u,
  AUTIB  = DataProcessing1SourceFixed | 0x80011400u,
  AUTDA  = DataProcessing1SourceFixed | 0x80011800u,
  AUTDB  = DataProcessing1SourceFixed | 0x80011C00u,
  PACIZA = DataProcessing1SourceFixed | 0x80012000u,
  PACIZB = DataProcessing1SourceFixed | 0x80012400u,
  PACDZA = DataProcessing1SourceFixed | 0x80012800u,
  PACDZB = DataProcessing1SourceFixed | 0x80012C00u,
  AUTIZA = DataProcessing1SourceFixed | 0x80013000u,
  AUTIZB = DataProcessing1SourceFixed | 0x80013400u,
  AUTDZA = DataProcessing1SourceFixed | 0x80013800u,
  AUTDZB = DataProcessing1SourceFixed | 0x80013C00u,
  XPACI  = DataProcessing1SourceFixed | 0x80014000u,
  XPACD  = DataProcessing1SourceFixed | 0x80014400u
};

// Data processing 2 source.
enum DataProcessing2SourceOp : uint32_t {
  DataProcessing2SourceFixed = 0x1AC00000u,
  DataProcessing2SourceFMask = 0x5FE00000u,
  DataProcessing2SourceMask  = 0xFFE0FC00u,
  UDIV_w  = DataProcessing2SourceFixed | 0x00000800u,
  UDIV_x  = DataProcessing2SourceFixed | 0x80000800u,
  UDIV    = UDIV_w,
  SDIV_w  = DataProcessing2SourceFixed | 0x00000C00u,
  SDIV_x  = DataProcessing2SourceFixed | 0x80000C00u,
  SDIV    = SDIV_w,
  LSLV_w  = DataProcessing2SourceFixed | 0x00002000u,
  LSLV_x  = DataProcessing2SourceFixed | 0x80002000u,
  LSLV    = LSLV_w,
  LSRV_w  = DataProcessing2SourceFixed | 0x00002400u,
  LSRV_x  = DataProcessing2SourceFixed | 0x80002400u,
  LSRV    = LSRV_w,
  ASRV_w  = DataProcessing2SourceFixed | 0x00002800u,
  ASRV_x  = DataProcessing2SourceFixed | 0x80002800u,
  ASRV    = ASRV_w,
  RORV_w  = DataProcessing2SourceFixed | 0x00002C00u,
  RORV_x  = DataProcessing2SourceFixed | 0x80002C00u,
  RORV    = RORV_w,
  PACGA   = DataProcessing2SourceFixed | SixtyFourBits | 0x00003000u,
  CRC32B  = DataProcessing2SourceFixed | 0x00004000u,
  CRC32H  = DataProcessing2SourceFixed | 0x00004400u,
  CRC32W  = DataProcessing2SourceFixed | 0x00004800u,
  CRC32X  = DataProcessing2SourceFixed | SixtyFourBits | 0x00004C00u,
  CRC32CB = DataProcessing2SourceFixed | 0x00005000u,
  CRC32CH = DataProcessing2SourceFixed | 0x00005400u,
  CRC32CW = DataProcessing2SourceFixed | 0x00005800u,
  CRC32CX = DataProcessing2SourceFixed | SixtyFourBits | 0x00005C00u
};

// Data processing 3 source.
enum DataProcessing3SourceOp : uint32_t {
  DataProcessing3SourceFixed = 0x1B000000u,
  DataProcessing3SourceFMask = 0x1F000000u,
  DataProcessing3SourceMask  = 0xFFE08000u,
  MADD_w                     = DataProcessing3SourceFixed | 0x00000000u,
  MADD_x                     = DataProcessing3SourceFixed | 0x80000000u,
  MADD                       = MADD_w,
  MSUB_w                     = DataProcessing3SourceFixed | 0x00008000u,
  MSUB_x                     = DataProcessing3SourceFixed | 0x80008000u,
  MSUB                       = MSUB_w,
  SMADDL_x                   = DataProcessing3SourceFixed | 0x80200000u,
  SMSUBL_x                   = DataProcessing3SourceFixed | 0x80208000u,
  SMULH_x                    = DataProcessing3SourceFixed | 0x80400000u,
  UMADDL_x                   = DataProcessing3SourceFixed | 0x80A00000u,
  UMSUBL_x                   = DataProcessing3SourceFixed | 0x80A08000u,
  UMULH_x                    = DataProcessing3SourceFixed | 0x80C00000u
};

// Floating point compare.
enum FPCompareOp : uint32_t {
  FPCompareFixed = 0x1E202000u,
  FPCompareFMask = 0x5F203C00u,
  FPCompareMask  = 0xFFE0FC1Fu,
  FCMP_h         = FPCompareFixed | FP16 | 0x00000000u,
  FCMP_s         = FPCompareFixed | 0x00000000u,
  FCMP_d         = FPCompareFixed | FP64 | 0x00000000u,
  FCMP           = FCMP_s,
  FCMP_h_zero    = FPCompareFixed | FP16 | 0x00000008u,
  FCMP_s_zero    = FPCompareFixed | 0x00000008u,
  FCMP_d_zero    = FPCompareFixed | FP64 | 0x00000008u,
  FCMP_zero      = FCMP_s_zero,
  FCMPE_h        = FPCompareFixed | FP16 | 0x00000010u,
  FCMPE_s        = FPCompareFixed | 0x00000010u,
  FCMPE_d        = FPCompareFixed | FP64 | 0x00000010u,
  FCMPE          = FCMPE_s,
  FCMPE_h_zero   = FPCompareFixed | FP16 | 0x00000018u,
  FCMPE_s_zero   = FPCompareFixed | 0x00000018u,
  FCMPE_d_zero   = FPCompareFixed | FP64 | 0x00000018u,
  FCMPE_zero     = FCMPE_s_zero
};

// Floating point conditional compare.
enum FPConditionalCompareOp : uint32_t {
  FPConditionalCompareFixed = 0x1E200400u,
  FPConditionalCompareFMask = 0x5F200C00u,
  FPConditionalCompareMask  = 0xFFE00C10u,
  FCCMP_h                   = FPConditionalCompareFixed | FP16 | 0x00000000u,
  FCCMP_s                   = FPConditionalCompareFixed | 0x00000000u,
  FCCMP_d                   = FPConditionalCompareFixed | FP64 | 0x00000000u,
  FCCMP                     = FCCMP_s,
  FCCMPE_h                  = FPConditionalCompareFixed | FP16 | 0x00000010u,
  FCCMPE_s                  = FPConditionalCompareFixed | 0x00000010u,
  FCCMPE_d                  = FPConditionalCompareFixed | FP64 | 0x00000010u,
  FCCMPE                    = FCCMPE_s
};

// Floating point conditional select.
enum FPConditionalSelectOp : uint32_t {
  FPConditionalSelectFixed = 0x1E200C00u,
  FPConditionalSelectFMask = 0x5F200C00u,
  FPConditionalSelectMask  = 0xFFE00C00u,
  FCSEL_h                  = FPConditionalSelectFixed | FP16 | 0x00000000u,
  FCSEL_s                  = FPConditionalSelectFixed | 0x00000000u,
  FCSEL_d                  = FPConditionalSelectFixed | FP64 | 0x00000000u,
  FCSEL                    = FCSEL_s
};

// Floating point immediate.
enum FPImmediateOp : uint32_t {
  FPImmediateFixed = 0x1E201000u,
  FPImmediateFMask = 0x5F201C00u,
  FPImmediateMask  = 0xFFE01C00u,
  FMOV_h_imm       = FPImmediateFixed | FP16 | 0x00000000u,
  FMOV_s_imm       = FPImmediateFixed | 0x00000000u,
  FMOV_d_imm       = FPImmediateFixed | FP64 | 0x00000000u
};

// Floating point data processing 1 source.
enum FPDataProcessing1SourceOp : uint32_t {
  FPDataProcessing1SourceFixed = 0x1E204000u,
  FPDataProcessing1SourceFMask = 0x5F207C00u,
  FPDataProcessing1SourceMask  = 0xFFFFFC00u,
  FMOV_h   = FPDataProcessing1SourceFixed | FP16 | 0x00000000u,
  FMOV_s   = FPDataProcessing1SourceFixed | 0x00000000u,
  FMOV_d   = FPDataProcessing1SourceFixed | FP64 | 0x00000000u,
  FMOV     = FMOV_s,
  FABS_h   = FPDataProcessing1SourceFixed | FP16 | 0x00008000u,
  FABS_s   = FPDataProcessing1SourceFixed | 0x00008000u,
  FABS_d   = FPDataProcessing1SourceFixed | FP64 | 0x00008000u,
  FABS     = FABS_s,
  FNEG_h   = FPDataProcessing1SourceFixed | FP16 | 0x00010000u,
  FNEG_s   = FPDataProcessing1SourceFixed | 0x00010000u,
  FNEG_d   = FPDataProcessing1SourceFixed | FP64 | 0x00010000u,
  FNEG     = FNEG_s,
  FSQRT_h  = FPDataProcessing1SourceFixed | FP16 | 0x00018000u,
  FSQRT_s  = FPDataProcessing1SourceFixed | 0x00018000u,
  FSQRT_d  = FPDataProcessing1SourceFixed | FP64 | 0x00018000u,
  FSQRT    = FSQRT_s,
  FCVT_ds  = FPDataProcessing1SourceFixed | 0x00028000u,
  FCVT_sd  = FPDataProcessing1SourceFixed | FP64 | 0x00020000u,
  FCVT_hs  = FPDataProcessing1SourceFixed | 0x00038000u,
  FCVT_hd  = FPDataProcessing1SourceFixed | FP64 | 0x00038000u,
  FCVT_sh  = FPDataProcessing1SourceFixed | 0x00C20000u,
  FCVT_dh  = FPDataProcessing1SourceFixed | 0x00C28000u,
  FRINTN_h = FPDataProcessing1SourceFixed | FP16 | 0x00040000u,
  FRINTN_s = FPDataProcessing1SourceFixed | 0x00040000u,
  FRINTN_d = FPDataProcessing1SourceFixed | FP64 | 0x00040000u,
  FRINTN   = FRINTN_s,
  FRINTP_h = FPDataProcessing1SourceFixed | FP16 | 0x00048000u,
  FRINTP_s = FPDataProcessing1SourceFixed | 0x00048000u,
  FRINTP_d = FPDataProcessing1SourceFixed | FP64 | 0x00048000u,
  FRINTP   = FRINTP_s,
  FRINTM_h = FPDataProcessing1SourceFixed | FP16 | 0x00050000u,
  FRINTM_s = FPDataProcessing1SourceFixed | 0x00050000u,
  FRINTM_d = FPDataProcessing1SourceFixed | FP64 | 0x00050000u,
  FRINTM   = FRINTM_s,
  FRINTZ_h = FPDataProcessing1SourceFixed | FP16 | 0x00058000u,
  FRINTZ_s = FPDataProcessing1SourceFixed | 0x00058000u,
  FRINTZ_d = FPDataProcessing1SourceFixed | FP64 | 0x00058000u,
  FRINTZ   = FRINTZ_s,
  FRINTA_h = FPDataProcessing1SourceFixed | FP16 | 0x00060000u,
  FRINTA_s = FPDataProcessing1SourceFixed | 0x00060000u,
  FRINTA_d = FPDataProcessing1SourceFixed | FP64 | 0x00060000u,
  FRINTA   = FRINTA_s,
  FRINTX_h = FPDataProcessing1SourceFixed | FP16 | 0x00070000u,
  FRINTX_s = FPDataProcessing1SourceFixed | 0x00070000u,
  FRINTX_d = FPDataProcessing1SourceFixed | FP64 | 0x00070000u,
  FRINTX   = FRINTX_s,
  FRINTI_h = FPDataProcessing1SourceFixed | FP16 | 0x00078000u,
  FRINTI_s = FPDataProcessing1SourceFixed | 0x00078000u,
  FRINTI_d = FPDataProcessing1SourceFixed | FP64 | 0x00078000u,
  FRINTI   = FRINTI_s
};

// Floating point data processing 2 source.
enum FPDataProcessing2SourceOp : uint32_t {
  FPDataProcessing2SourceFixed = 0x1E200800u,
  FPDataProcessing2SourceFMask = 0x5F200C00u,
  FPDataProcessing2SourceMask  = 0xFFE0FC00u,
  FMUL     = FPDataProcessing2SourceFixed | 0x00000000u,
  FMUL_h   = FMUL | FP16,
  FMUL_s   = FMUL,
  FMUL_d   = FMUL | FP64,
  FDIV     = FPDataProcessing2SourceFixed | 0x00001000u,
  FDIV_h   = FDIV | FP16,
  FDIV_s   = FDIV,
  FDIV_d   = FDIV | FP64,
  FADD     = FPDataProcessing2SourceFixed | 0x00002000u,
  FADD_h   = FADD | FP16,
  FADD_s   = FADD,
  FADD_d   = FADD | FP64,
  FSUB     = FPDataProcessing2SourceFixed | 0x00003000u,
  FSUB_h   = FSUB | FP16,
  FSUB_s   = FSUB,
  FSUB_d   = FSUB | FP64,
  FMAX     = FPDataProcessing2SourceFixed | 0x00004000u,
  FMAX_h   = FMAX | FP16,
  FMAX_s   = FMAX,
  FMAX_d   = FMAX | FP64,
  FMIN     = FPDataProcessing2SourceFixed | 0x00005000u,
  FMIN_h   = FMIN | FP16,
  FMIN_s   = FMIN,
  FMIN_d   = FMIN | FP64,
  FMAXNM   = FPDataProcessing2SourceFixed | 0x00006000u,
  FMAXNM_h = FMAXNM | FP16,
  FMAXNM_s = FMAXNM,
  FMAXNM_d = FMAXNM | FP64,
  FMINNM   = FPDataProcessing2SourceFixed | 0x00007000u,
  FMINNM_h = FMINNM | FP16,
  FMINNM_s = FMINNM,
  FMINNM_d = FMINNM | FP64,
  FNMUL    = FPDataProcessing2SourceFixed | 0x00008000u,
  FNMUL_h  = FNMUL | FP16,
  FNMUL_s  = FNMUL,
  FNMUL_d  = FNMUL | FP64
};

// Floating point data processing 3 source.
enum FPDataProcessing3SourceOp : uint32_t {
  FPDataProcessing3SourceFixed = 0x1F000000u,
  FPDataProcessing3SourceFMask = 0x5F000000u,
  FPDataProcessing3SourceMask  = 0xFFE08000u,
  FMADD_h                      = FPDataProcessing3SourceFixed | 0x00C00000u,
  FMSUB_h                      = FPDataProcessing3SourceFixed | 0x00C08000u,
  FNMADD_h                     = FPDataProcessing3SourceFixed | 0x00E00000u,
  FNMSUB_h                     = FPDataProcessing3SourceFixed | 0x00E08000u,
  FMADD_s                      = FPDataProcessing3SourceFixed | 0x00000000u,
  FMSUB_s                      = FPDataProcessing3SourceFixed | 0x00008000u,
  FNMADD_s                     = FPDataProcessing3SourceFixed | 0x00200000u,
  FNMSUB_s                     = FPDataProcessing3SourceFixed | 0x00208000u,
  FMADD_d                      = FPDataProcessing3SourceFixed | 0x00400000u,
  FMSUB_d                      = FPDataProcessing3SourceFixed | 0x00408000u,
  FNMADD_d                     = FPDataProcessing3SourceFixed | 0x00600000u,
  FNMSUB_d                     = FPDataProcessing3SourceFixed | 0x00608000u
};

// Conversion between floating point and integer.
enum FPIntegerConvertOp : uint32_t {
  FPIntegerConvertFixed = 0x1E200000u,
  FPIntegerConvertFMask = 0x5F20FC00u,
  FPIntegerConvertMask  = 0xFFFFFC00u,
  FCVTNS    = FPIntegerConvertFixed | 0x00000000u,
  FCVTNS_wh = FCVTNS | FP16,
  FCVTNS_xh = FCVTNS | SixtyFourBits | FP16,
  FCVTNS_ws = FCVTNS,
  FCVTNS_xs = FCVTNS | SixtyFourBits,
  FCVTNS_wd = FCVTNS | FP64,
  FCVTNS_xd = FCVTNS | SixtyFourBits | FP64,
  FCVTNU    = FPIntegerConvertFixed | 0x00010000u,
  FCVTNU_wh = FCVTNU | FP16,
  FCVTNU_xh = FCVTNU | SixtyFourBits | FP16,
  FCVTNU_ws = FCVTNU,
  FCVTNU_xs = FCVTNU | SixtyFourBits,
  FCVTNU_wd = FCVTNU | FP64,
  FCVTNU_xd = FCVTNU | SixtyFourBits | FP64,
  FCVTPS    = FPIntegerConvertFixed | 0x00080000u,
  FCVTPS_wh = FCVTPS | FP16,
  FCVTPS_xh = FCVTPS | SixtyFourBits | FP16,
  FCVTPS_ws = FCVTPS,
  FCVTPS_xs = FCVTPS | SixtyFourBits,
  FCVTPS_wd = FCVTPS | FP64,
  FCVTPS_xd = FCVTPS | SixtyFourBits | FP64,
  FCVTPU    = FPIntegerConvertFixed | 0x00090000u,
  FCVTPU_wh = FCVTPU | FP16,
  FCVTPU_xh = FCVTPU | SixtyFourBits | FP16,
  FCVTPU_ws = FCVTPU,
  FCVTPU_xs = FCVTPU | SixtyFourBits,
  FCVTPU_wd = FCVTPU | FP64,
  FCVTPU_xd = FCVTPU | SixtyFourBits | FP64,
  FCVTMS    = FPIntegerConvertFixed | 0x00100000u,
  FCVTMS_wh = FCVTMS | FP16,
  FCVTMS_xh = FCVTMS | SixtyFourBits | FP16,
  FCVTMS_ws = FCVTMS,
  FCVTMS_xs = FCVTMS | SixtyFourBits,
  FCVTMS_wd = FCVTMS | FP64,
  FCVTMS_xd = FCVTMS | SixtyFourBits | FP64,
  FCVTMU    = FPIntegerConvertFixed | 0x00110000u,
  FCVTMU_wh = FCVTMU | FP16,
  FCVTMU_xh = FCVTMU | SixtyFourBits | FP16,
  FCVTMU_ws = FCVTMU,
  FCVTMU_xs = FCVTMU | SixtyFourBits,
  FCVTMU_wd = FCVTMU | FP64,
  FCVTMU_xd = FCVTMU | SixtyFourBits | FP64,
  FCVTZS    = FPIntegerConvertFixed | 0x00180000u,
  FCVTZS_wh = FCVTZS | FP16,
  FCVTZS_xh = FCVTZS | SixtyFourBits | FP16,
  FCVTZS_ws = FCVTZS,
  FCVTZS_xs = FCVTZS | SixtyFourBits,
  FCVTZS_wd = FCVTZS | FP64,
  FCVTZS_xd = FCVTZS | SixtyFourBits | FP64,
  FCVTZU    = FPIntegerConvertFixed | 0x00190000u,
  FCVTZU_wh = FCVTZU | FP16,
  FCVTZU_xh = FCVTZU | SixtyFourBits | FP16,
  FCVTZU_ws = FCVTZU,
  FCVTZU_xs = FCVTZU | SixtyFourBits,
  FCVTZU_wd = FCVTZU | FP64,
  FCVTZU_xd = FCVTZU | SixtyFourBits | FP64,
  SCVTF     = FPIntegerConvertFixed | 0x00020000u,
  SCVTF_hw  = SCVTF | FP16,
  SCVTF_hx  = SCVTF | SixtyFourBits | FP16,
  SCVTF_sw  = SCVTF,
  SCVTF_sx  = SCVTF | SixtyFourBits,
  SCVTF_dw  = SCVTF | FP64,
  SCVTF_dx  = SCVTF | SixtyFourBits | FP64,
  UCVTF     = FPIntegerConvertFixed | 0x00030000u,
  UCVTF_hw  = UCVTF | FP16,
  UCVTF_hx  = UCVTF | SixtyFourBits | FP16,
  UCVTF_sw  = UCVTF,
  UCVTF_sx  = UCVTF | SixtyFourBits,
  UCVTF_dw  = UCVTF | FP64,
  UCVTF_dx  = UCVTF | SixtyFourBits | FP64,
  FCVTAS    = FPIntegerConvertFixed | 0x00040000u,
  FCVTAS_wh = FCVTAS | FP16,
  FCVTAS_xh = FCVTAS | SixtyFourBits | FP16,
  FCVTAS_ws = FCVTAS,
  FCVTAS_xs = FCVTAS | SixtyFourBits,
  FCVTAS_wd = FCVTAS | FP64,
  FCVTAS_xd = FCVTAS | SixtyFourBits | FP64,
  FCVTAU    = FPIntegerConvertFixed | 0x00050000u,
  FCVTAU_wh = FCVTAU | FP16,
  FCVTAU_xh = FCVTAU | SixtyFourBits | FP16,
  FCVTAU_ws = FCVTAU,
  FCVTAU_xs = FCVTAU | SixtyFourBits,
  FCVTAU_wd = FCVTAU | FP64,
  FCVTAU_xd = FCVTAU | SixtyFourBits | FP64,
  FMOV_wh   = FPIntegerConvertFixed | 0x00060000u | FP16,
  FMOV_hw   = FPIntegerConvertFixed | 0x00070000u | FP16,
  FMOV_xh   = FMOV_wh | SixtyFourBits,
  FMOV_hx   = FMOV_hw | SixtyFourBits,
  FMOV_ws   = FPIntegerConvertFixed | 0x00060000u,
  FMOV_sw   = FPIntegerConvertFixed | 0x00070000u,
  FMOV_xd   = FMOV_ws | SixtyFourBits | FP64,
  FMOV_dx   = FMOV_sw | SixtyFourBits | FP64,
  FMOV_d1_x = FPIntegerConvertFixed | SixtyFourBits | 0x008F0000u,
  FMOV_x_d1 = FPIntegerConvertFixed | SixtyFourBits | 0x008E0000u,
  FJCVTZS   = FPIntegerConvertFixed | FP64 | 0x001E0000
};

// Conversion between fixed point and floating point.
enum FPFixedPointConvertOp : uint32_t {
  FPFixedPointConvertFixed = 0x1E000000u,
  FPFixedPointConvertFMask = 0x5F200000u,
  FPFixedPointConvertMask  = 0xFFFF0000u,
  FCVTZS_fixed    = FPFixedPointConvertFixed | 0x00180000u,
  FCVTZS_wh_fixed = FCVTZS_fixed | FP16,
  FCVTZS_xh_fixed = FCVTZS_fixed | SixtyFourBits | FP16,
  FCVTZS_ws_fixed = FCVTZS_fixed,
  FCVTZS_xs_fixed = FCVTZS_fixed | SixtyFourBits,
  FCVTZS_wd_fixed = FCVTZS_fixed | FP64,
  FCVTZS_xd_fixed = FCVTZS_fixed | SixtyFourBits | FP64,
  FCVTZU_fixed    = FPFixedPointConvertFixed | 0x00190000u,
  FCVTZU_wh_fixed = FCVTZU_fixed | FP16,
  FCVTZU_xh_fixed = FCVTZU_fixed | SixtyFourBits | FP16,
  FCVTZU_ws_fixed = FCVTZU_fixed,
  FCVTZU_xs_fixed = FCVTZU_fixed | SixtyFourBits,
  FCVTZU_wd_fixed = FCVTZU_fixed | FP64,
  FCVTZU_xd_fixed = FCVTZU_fixed | SixtyFourBits | FP64,
  SCVTF_fixed     = FPFixedPointConvertFixed | 0x00020000u,
  SCVTF_hw_fixed  = SCVTF_fixed | FP16,
  SCVTF_hx_fixed  = SCVTF_fixed | SixtyFourBits | FP16,
  SCVTF_sw_fixed  = SCVTF_fixed,
  SCVTF_sx_fixed  = SCVTF_fixed | SixtyFourBits,
  SCVTF_dw_fixed  = SCVTF_fixed | FP64,
  SCVTF_dx_fixed  = SCVTF_fixed | SixtyFourBits | FP64,
  UCVTF_fixed     = FPFixedPointConvertFixed | 0x00030000u,
  UCVTF_hw_fixed  = UCVTF_fixed | FP16,
  UCVTF_hx_fixed  = UCVTF_fixed | SixtyFourBits | FP16,
  UCVTF_sw_fixed  = UCVTF_fixed,
  UCVTF_sx_fixed  = UCVTF_fixed | SixtyFourBits,
  UCVTF_dw_fixed  = UCVTF_fixed | FP64,
  UCVTF_dx_fixed  = UCVTF_fixed | SixtyFourBits | FP64
};

// Crypto - two register SHA.
enum Crypto2RegSHAOp : uint32_t {
  Crypto2RegSHAFixed = 0x5E280800u,
  Crypto2RegSHAFMask = 0xFF3E0C00u
};

// Crypto - three register SHA.
enum Crypto3RegSHAOp : uint32_t {
  Crypto3RegSHAFixed = 0x5E000000u,
  Crypto3RegSHAFMask = 0xFF208C00u
};

// Crypto - AES.
enum CryptoAESOp : uint32_t {
  CryptoAESFixed = 0x4E280800u,
  CryptoAESFMask = 0xFF3E0C00u
};

// NEON instructions with two register operands.
enum NEON2RegMiscOp : uint32_t {
  NEON2RegMiscFixed = 0x0E200800u,
  NEON2RegMiscFMask = 0x9F3E0C00u,
  NEON2RegMiscMask  = 0xBF3FFC00u,
  NEON2RegMiscUBit  = 0x20000000u,
  NEON_REV64     = NEON2RegMiscFixed | 0x00000000u,
  NEON_REV32     = NEON2RegMiscFixed | 0x20000000u,
  NEON_REV16     = NEON2RegMiscFixed | 0x00001000u,
  NEON_SADDLP    = NEON2RegMiscFixed | 0x00002000u,
  NEON_UADDLP    = NEON_SADDLP | NEON2RegMiscUBit,
  NEON_SUQADD    = NEON2RegMiscFixed | 0x00003000u,
  NEON_USQADD    = NEON_SUQADD | NEON2RegMiscUBit,
  NEON_CLS       = NEON2RegMiscFixed | 0x00004000u,
  NEON_CLZ       = NEON2RegMiscFixed | 0x20004000u,
  NEON_CNT       = NEON2RegMiscFixed | 0x00005000u,
  NEON_RBIT_NOT  = NEON2RegMiscFixed | 0x20005000u,
  NEON_SADALP    = NEON2RegMiscFixed | 0x00006000u,
  NEON_UADALP    = NEON_SADALP | NEON2RegMiscUBit,
  NEON_SQABS     = NEON2RegMiscFixed | 0x00007000u,
  NEON_SQNEG     = NEON2RegMiscFixed | 0x20007000u,
  NEON_CMGT_zero = NEON2RegMiscFixed | 0x00008000u,
  NEON_CMGE_zero = NEON2RegMiscFixed | 0x20008000u,
  NEON_CMEQ_zero = NEON2RegMiscFixed | 0x00009000u,
  NEON_CMLE_zero = NEON2RegMiscFixed | 0x20009000u,
  NEON_CMLT_zero = NEON2RegMiscFixed | 0x0000A000u,
  NEON_ABS       = NEON2RegMiscFixed | 0x0000B000u,
  NEON_NEG       = NEON2RegMiscFixed | 0x2000B000u,
  NEON_XTN       = NEON2RegMiscFixed | 0x00012000u,
  NEON_SQXTUN    = NEON2RegMiscFixed | 0x20012000u,
  NEON_SHLL      = NEON2RegMiscFixed | 0x20013000u,
  NEON_SQXTN     = NEON2RegMiscFixed | 0x00014000u,
  NEON_UQXTN     = NEON_SQXTN | NEON2RegMiscUBit,

  NEON2RegMiscOpcode = 0x0001F000u,
  NEON_RBIT_NOT_opcode = NEON_RBIT_NOT & NEON2RegMiscOpcode,
  NEON_NEG_opcode = NEON_NEG & NEON2RegMiscOpcode,
  NEON_XTN_opcode = NEON_XTN & NEON2RegMiscOpcode,
  NEON_UQXTN_opcode = NEON_UQXTN & NEON2RegMiscOpcode,

  // These instructions use only one bit of the size field. The other bit is
  // used to distinguish between instructions.
  NEON2RegMiscFPMask = NEON2RegMiscMask | 0x00800000u,
  NEON_FABS   = NEON2RegMiscFixed | 0x0080F000u,
  NEON_FNEG   = NEON2RegMiscFixed | 0x2080F000u,
  NEON_FCVTN  = NEON2RegMiscFixed | 0x00016000u,
  NEON_FCVTXN = NEON2RegMiscFixed | 0x20016000u,
  NEON_FCVTL  = NEON2RegMiscFixed | 0x00017000u,
  NEON_FRINTN = NEON2RegMiscFixed | 0x00018000u,
  NEON_FRINTA = NEON2RegMiscFixed | 0x20018000u,
  NEON_FRINTP = NEON2RegMiscFixed | 0x00818000u,
  NEON_FRINTM = NEON2RegMiscFixed | 0x00019000u,
  NEON_FRINTX = NEON2RegMiscFixed | 0x20019000u,
  NEON_FRINTZ = NEON2RegMiscFixed | 0x00819000u,
  NEON_FRINTI = NEON2RegMiscFixed | 0x20819000u,
  NEON_FCVTNS = NEON2RegMiscFixed | 0x0001A000u,
  NEON_FCVTNU = NEON_FCVTNS | NEON2RegMiscUBit,
  NEON_FCVTPS = NEON2RegMiscFixed | 0x0081A000u,
  NEON_FCVTPU = NEON_FCVTPS | NEON2RegMiscUBit,
  NEON_FCVTMS = NEON2RegMiscFixed | 0x0001B000u,
  NEON_FCVTMU = NEON_FCVTMS | NEON2RegMiscUBit,
  NEON_FCVTZS = NEON2RegMiscFixed | 0x0081B000u,
  NEON_FCVTZU = NEON_FCVTZS | NEON2RegMiscUBit,
  NEON_FCVTAS = NEON2RegMiscFixed | 0x0001C000u,
  NEON_FCVTAU = NEON_FCVTAS | NEON2RegMiscUBit,
  NEON_FSQRT  = NEON2RegMiscFixed | 0x2081F000u,
  NEON_SCVTF  = NEON2RegMiscFixed | 0x0001D000u,
  NEON_UCVTF  = NEON_SCVTF | NEON2RegMiscUBit,
  NEON_URSQRTE = NEON2RegMiscFixed | 0x2081C000u,
  NEON_URECPE  = NEON2RegMiscFixed | 0x0081C000u,
  NEON_FRSQRTE = NEON2RegMiscFixed | 0x2081D000u,
  NEON_FRECPE  = NEON2RegMiscFixed | 0x0081D000u,
  NEON_FCMGT_zero = NEON2RegMiscFixed | 0x0080C000u,
  NEON_FCMGE_zero = NEON2RegMiscFixed | 0x2080C000u,
  NEON_FCMEQ_zero = NEON2RegMiscFixed | 0x0080D000u,
  NEON_FCMLE_zero = NEON2RegMiscFixed | 0x2080D000u,
  NEON_FCMLT_zero = NEON2RegMiscFixed | 0x0080E000u,

  NEON_FCVTL_opcode = NEON_FCVTL & NEON2RegMiscOpcode,
  NEON_FCVTN_opcode = NEON_FCVTN & NEON2RegMiscOpcode
};

// NEON instructions with two register operands (FP16).
enum NEON2RegMiscFP16Op : uint32_t {
  NEON2RegMiscFP16Fixed = 0x0E780800u,
  NEON2RegMiscFP16FMask = 0x9F7E0C00u,
  NEON2RegMiscFP16Mask  = 0xBFFFFC00u,
  NEON_FRINTN_H     = NEON2RegMiscFP16Fixed | 0x00018000u,
  NEON_FRINTM_H     = NEON2RegMiscFP16Fixed | 0x00019000u,
  NEON_FCVTNS_H     = NEON2RegMiscFP16Fixed | 0x0001A000u,
  NEON_FCVTMS_H     = NEON2RegMiscFP16Fixed | 0x0001B000u,
  NEON_FCVTAS_H     = NEON2RegMiscFP16Fixed | 0x0001C000u,
  NEON_SCVTF_H      = NEON2RegMiscFP16Fixed | 0x0001D000u,
  NEON_FCMGT_H_zero = NEON2RegMiscFP16Fixed | 0x0080C000u,
  NEON_FCMEQ_H_zero = NEON2RegMiscFP16Fixed | 0x0080D000u,
  NEON_FCMLT_H_zero = NEON2RegMiscFP16Fixed | 0x0080E000u,
  NEON_FABS_H       = NEON2RegMiscFP16Fixed | 0x0080F000u,
  NEON_FRINTP_H     = NEON2RegMiscFP16Fixed | 0x00818000u,
  NEON_FRINTZ_H     = NEON2RegMiscFP16Fixed | 0x00819000u,
  NEON_FCVTPS_H     = NEON2RegMiscFP16Fixed | 0x0081A000u,
  NEON_FCVTZS_H     = NEON2RegMiscFP16Fixed | 0x0081B000u,
  NEON_FRECPE_H     = NEON2RegMiscFP16Fixed | 0x0081D000u,
  NEON_FRINTA_H     = NEON2RegMiscFP16Fixed | 0x20018000u,
  NEON_FRINTX_H     = NEON2RegMiscFP16Fixed | 0x20019000u,
  NEON_FCVTNU_H     = NEON2RegMiscFP16Fixed | 0x2001A000u,
  NEON_FCVTMU_H     = NEON2RegMiscFP16Fixed | 0x2001B000u,
  NEON_FCVTAU_H     = NEON2RegMiscFP16Fixed | 0x2001C000u,
  NEON_UCVTF_H      = NEON2RegMiscFP16Fixed | 0x2001D000u,
  NEON_FCMGE_H_zero = NEON2RegMiscFP16Fixed | 0x2080C000u,
  NEON_FCMLE_H_zero = NEON2RegMiscFP16Fixed | 0x2080D000u,
  NEON_FNEG_H       = NEON2RegMiscFP16Fixed | 0x2080F000u,
  NEON_FRINTI_H     = NEON2RegMiscFP16Fixed | 0x20819000u,
  NEON_FCVTPU_H     = NEON2RegMiscFP16Fixed | 0x2081A000u,
  NEON_FCVTZU_H     = NEON2RegMiscFP16Fixed | 0x2081B000u,
  NEON_FRSQRTE_H    = NEON2RegMiscFP16Fixed | 0x2081D000u,
  NEON_FSQRT_H      = NEON2RegMiscFP16Fixed | 0x2081F000u
};

// NEON instructions with three same-type operands.
enum NEON3SameOp : uint32_t {
  NEON3SameFixed = 0x0E200400u,
  NEON3SameFMask = 0x9F200400u,
  NEON3SameMask =  0xBF20FC00u,
  NEON3SameUBit =  0x20000000u,
  NEON_ADD    = NEON3SameFixed | 0x00008000u,
  NEON_ADDP   = NEON3SameFixed | 0x0000B800u,
  NEON_SHADD  = NEON3SameFixed | 0x00000000u,
  NEON_SHSUB  = NEON3SameFixed | 0x00002000u,
  NEON_SRHADD = NEON3SameFixed | 0x00001000u,
  NEON_CMEQ   = NEON3SameFixed | NEON3SameUBit | 0x00008800u,
  NEON_CMGE   = NEON3SameFixed | 0x00003800u,
  NEON_CMGT   = NEON3SameFixed | 0x00003000u,
  NEON_CMHI   = NEON3SameFixed | NEON3SameUBit | NEON_CMGT,
  NEON_CMHS   = NEON3SameFixed | NEON3SameUBit | NEON_CMGE,
  NEON_CMTST  = NEON3SameFixed | 0x00008800u,
  NEON_MLA    = NEON3SameFixed | 0x00009000u,
  NEON_MLS    = NEON3SameFixed | 0x20009000u,
  NEON_MUL    = NEON3SameFixed | 0x00009800u,
  NEON_PMUL   = NEON3SameFixed | 0x20009800u,
  NEON_SRSHL  = NEON3SameFixed | 0x00005000u,
  NEON_SQSHL  = NEON3SameFixed | 0x00004800u,
  NEON_SQRSHL = NEON3SameFixed | 0x00005800u,
  NEON_SSHL   = NEON3SameFixed | 0x00004000u,
  NEON_SMAX   = NEON3SameFixed | 0x00006000u,
  NEON_SMAXP  = NEON3SameFixed | 0x0000A000u,
  NEON_SMIN   = NEON3SameFixed | 0x00006800u,
  NEON_SMINP  = NEON3SameFixed | 0x0000A800u,
  NEON_SABD   = NEON3SameFixed | 0x00007000u,
  NEON_SABA   = NEON3SameFixed | 0x00007800u,
  NEON_UABD   = NEON3SameFixed | NEON3SameUBit | NEON_SABD,
  NEON_UABA   = NEON3SameFixed | NEON3SameUBit | NEON_SABA,
  NEON_SQADD  = NEON3SameFixed | 0x00000800u,
  NEON_SQSUB  = NEON3SameFixed | 0x00002800u,
  NEON_SUB    = NEON3SameFixed | NEON3SameUBit | 0x00008000u,
  NEON_UHADD  = NEON3SameFixed | NEON3SameUBit | NEON_SHADD,
  NEON_UHSUB  = NEON3SameFixed | NEON3SameUBit | NEON_SHSUB,
  NEON_URHADD = NEON3SameFixed | NEON3SameUBit | NEON_SRHADD,
  NEON_UMAX   = NEON3SameFixed | NEON3SameUBit | NEON_SMAX,
  NEON_UMAXP  = NEON3SameFixed | NEON3SameUBit | NEON_SMAXP,
  NEON_UMIN   = NEON3SameFixed | NEON3SameUBit | NEON_SMIN,
  NEON_UMINP  = NEON3SameFixed | NEON3SameUBit | NEON_SMINP,
  NEON_URSHL  = NEON3SameFixed | NEON3SameUBit | NEON_SRSHL,
  NEON_UQADD  = NEON3SameFixed | NEON3SameUBit | NEON_SQADD,
  NEON_UQRSHL = NEON3SameFixed | NEON3SameUBit | NEON_SQRSHL,
  NEON_UQSHL  = NEON3SameFixed | NEON3SameUBit | NEON_SQSHL,
  NEON_UQSUB  = NEON3SameFixed | NEON3SameUBit | NEON_SQSUB,
  NEON_USHL   = NEON3SameFixed | NEON3SameUBit | NEON_SSHL,
  NEON_SQDMULH  = NEON3SameFixed | 0x0000B000u,
  NEON_SQRDMULH = NEON3SameFixed | 0x2000B000u,

  // NEON floating point instructions with three same-type operands.
  NEON3SameFPFixed = NEON3SameFixed | 0x0000C000u,
  NEON3SameFPFMask = NEON3SameFMask | 0x0000C000u,
  NEON3SameFPMask = NEON3SameMask | 0x00800000u,
  NEON_FADD    = NEON3SameFixed | 0x0000D000u,
  NEON_FSUB    = NEON3SameFixed | 0x0080D000u,
  NEON_FMUL    = NEON3SameFixed | 0x2000D800u,
  NEON_FDIV    = NEON3SameFixed | 0x2000F800u,
  NEON_FMAX    = NEON3SameFixed | 0x0000F000u,
  NEON_FMAXNM  = NEON3SameFixed | 0x0000C000u,
  NEON_FMAXP   = NEON3SameFixed | 0x2000F000u,
  NEON_FMAXNMP = NEON3SameFixed | 0x2000C000u,
  NEON_FMIN    = NEON3SameFixed | 0x0080F000u,
  NEON_FMINNM  = NEON3SameFixed | 0x0080C000u,
  NEON_FMINP   = NEON3SameFixed | 0x2080F000u,
  NEON_FMINNMP = NEON3SameFixed | 0x2080C000u,
  NEON_FMLA    = NEON3SameFixed | 0x0000C800u,
  NEON_FMLS    = NEON3SameFixed | 0x0080C800u,
  NEON_FMULX   = NEON3SameFixed | 0x0000D800u,
  NEON_FRECPS  = NEON3SameFixed | 0x0000F800u,
  NEON_FRSQRTS = NEON3SameFixed | 0x0080F800u,
  NEON_FABD    = NEON3SameFixed | 0x2080D000u,
  NEON_FADDP   = NEON3SameFixed | 0x2000D000u,
  NEON_FCMEQ   = NEON3SameFixed | 0x0000E000u,
  NEON_FCMGE   = NEON3SameFixed | 0x2000E000u,
  NEON_FCMGT   = NEON3SameFixed | 0x2080E000u,
  NEON_FACGE   = NEON3SameFixed | 0x2000E800u,
  NEON_FACGT   = NEON3SameFixed | 0x2080E800u,

  // NEON logical instructions with three same-type operands.
  NEON3SameLogicalFixed = NEON3SameFixed | 0x00001800u,
  NEON3SameLogicalFMask = NEON3SameFMask | 0x0000F800u,
  NEON3SameLogicalMask = 0xBFE0FC00u,
  NEON3SameLogicalFormatMask = NEON_Q,
  NEON_AND = NEON3SameLogicalFixed | 0x00000000u,
  NEON_ORR = NEON3SameLogicalFixed | 0x00A00000u,
  NEON_ORN = NEON3SameLogicalFixed | 0x00C00000u,
  NEON_EOR = NEON3SameLogicalFixed | 0x20000000u,
  NEON_BIC = NEON3SameLogicalFixed | 0x00400000u,
  NEON_BIF = NEON3SameLogicalFixed | 0x20C00000u,
  NEON_BIT = NEON3SameLogicalFixed | 0x20800000u,
  NEON_BSL = NEON3SameLogicalFixed | 0x20400000u
};


enum NEON3SameFP16 : uint32_t {
  NEON3SameFP16Fixed = 0x0E400400u,
  NEON3SameFP16FMask = 0x9F60C400u,
  NEON3SameFP16Mask =  0xBFE0FC00u,
  NEON_FMAXNM_H  = NEON3SameFP16Fixed | 0x00000000u,
  NEON_FMLA_H    = NEON3SameFP16Fixed | 0x00000800u,
  NEON_FADD_H    = NEON3SameFP16Fixed | 0x00001000u,
  NEON_FMULX_H   = NEON3SameFP16Fixed | 0x00001800u,
  NEON_FCMEQ_H   = NEON3SameFP16Fixed | 0x00002000u,
  NEON_FMAX_H    = NEON3SameFP16Fixed | 0x00003000u,
  NEON_FRECPS_H  = NEON3SameFP16Fixed | 0x00003800u,
  NEON_FMINNM_H  = NEON3SameFP16Fixed | 0x00800000u,
  NEON_FMLS_H    = NEON3SameFP16Fixed | 0x00800800u,
  NEON_FSUB_H    = NEON3SameFP16Fixed | 0x00801000u,
  NEON_FMIN_H    = NEON3SameFP16Fixed | 0x00803000u,
  NEON_FRSQRTS_H = NEON3SameFP16Fixed | 0x00803800u,
  NEON_FMAXNMP_H = NEON3SameFP16Fixed | 0x20000000u,
  NEON_FADDP_H   = NEON3SameFP16Fixed | 0x20001000u,
  NEON_FMUL_H    = NEON3SameFP16Fixed | 0x20001800u,
  NEON_FCMGE_H   = NEON3SameFP16Fixed | 0x20002000u,
  NEON_FACGE_H   = NEON3SameFP16Fixed | 0x20002800u,
  NEON_FMAXP_H   = NEON3SameFP16Fixed | 0x20003000u,
  NEON_FDIV_H    = NEON3SameFP16Fixed | 0x20003800u,
  NEON_FMINNMP_H = NEON3SameFP16Fixed | 0x20800000u,
  NEON_FABD_H    = NEON3SameFP16Fixed | 0x20801000u,
  NEON_FCMGT_H   = NEON3SameFP16Fixed | 0x20802000u,
  NEON_FACGT_H   = NEON3SameFP16Fixed | 0x20802800u,
  NEON_FMINP_H   = NEON3SameFP16Fixed | 0x20803000u
};


// 'Extra' NEON instructions with three same-type operands.
enum NEON3SameExtraOp : uint32_t {
  NEON3SameExtraFixed = 0x0E008400u,
  NEON3SameExtraUBit = 0x20000000u,
  NEON3SameExtraFMask = 0x9E208400u,
  NEON3SameExtraMask = 0xBE20FC00u,
  NEON_SQRDMLAH = NEON3SameExtraFixed | NEON3SameExtraUBit,
  NEON_SQRDMLSH = NEON3SameExtraFixed | NEON3SameExtraUBit | 0x00000800u,
  NEON_SDOT = NEON3SameExtraFixed | 0x00001000u,
  NEON_UDOT = NEON3SameExtraFixed | NEON3SameExtraUBit | 0x00001000u,

  /* v8.3 Complex Numbers */
  NEON3SameExtraFCFixed = 0x2E00C400u,
  NEON3SameExtraFCFMask = 0xBF20C400u,
  // FCMLA fixes opcode<3:2>, and uses opcode<1:0> to encode <rotate>.
  NEON3SameExtraFCMLAMask = NEON3SameExtraFCFMask | 0x00006000u,
  NEON_FCMLA = NEON3SameExtraFCFixed,
  // FCADD fixes opcode<3:2, 0>, and uses opcode<1> to encode <rotate>.
  NEON3SameExtraFCADDMask = NEON3SameExtraFCFMask | 0x00006800u,
  NEON_FCADD = NEON3SameExtraFCFixed | 0x00002000u
  // Other encodings under NEON3SameExtraFCFMask are UNALLOCATED.
};

// NEON instructions with three different-type operands.
enum NEON3DifferentOp : uint32_t {
  NEON3DifferentFixed = 0x0E200000u,
  NEON3DifferentFMask = 0x9F200C00u,
  NEON3DifferentMask  = 0xFF20FC00u,
  NEON_ADDHN    = NEON3DifferentFixed | 0x00004000u,
  NEON_ADDHN2   = NEON_ADDHN | NEON_Q,
  NEON_PMULL    = NEON3DifferentFixed | 0x0000E000u,
  NEON_PMULL2   = NEON_PMULL | NEON_Q,
  NEON_RADDHN   = NEON3DifferentFixed | 0x20004000u,
  NEON_RADDHN2  = NEON_RADDHN | NEON_Q,
  NEON_RSUBHN   = NEON3DifferentFixed | 0x20006000u,
  NEON_RSUBHN2  = NEON_RSUBHN | NEON_Q,
  NEON_SABAL    = NEON3DifferentFixed | 0x00005000u,
  NEON_SABAL2   = NEON_SABAL | NEON_Q,
  NEON_SABDL    = NEON3DifferentFixed | 0x00007000u,
  NEON_SABDL2   = NEON_SABDL | NEON_Q,
  NEON_SADDL    = NEON3DifferentFixed | 0x00000000u,
  NEON_SADDL2   = NEON_SADDL | NEON_Q,
  NEON_SADDW    = NEON3DifferentFixed | 0x00001000u,
  NEON_SADDW2   = NEON_SADDW | NEON_Q,
  NEON_SMLAL    = NEON3DifferentFixed | 0x00008000u,
  NEON_SMLAL2   = NEON_SMLAL | NEON_Q,
  NEON_SMLSL    = NEON3DifferentFixed | 0x0000A000u,
  NEON_SMLSL2   = NEON_SMLSL | NEON_Q,
  NEON_SMULL    = NEON3DifferentFixed | 0x0000C000u,
  NEON_SMULL2   = NEON_SMULL | NEON_Q,
  NEON_SSUBL    = NEON3DifferentFixed | 0x00002000u,
  NEON_SSUBL2   = NEON_SSUBL | NEON_Q,
  NEON_SSUBW    = NEON3DifferentFixed | 0x00003000u,
  NEON_SSUBW2   = NEON_SSUBW | NEON_Q,
  NEON_SQDMLAL  = NEON3DifferentFixed | 0x00009000u,
  NEON_SQDMLAL2 = NEON_SQDMLAL | NEON_Q,
  NEON_SQDMLSL  = NEON3DifferentFixed | 0x0000B000u,
  NEON_SQDMLSL2 = NEON_SQDMLSL | NEON_Q,
  NEON_SQDMULL  = NEON3DifferentFixed | 0x0000D000u,
  NEON_SQDMULL2 = NEON_SQDMULL | NEON_Q,
  NEON_SUBHN    = NEON3DifferentFixed | 0x00006000u,
  NEON_SUBHN2   = NEON_SUBHN | NEON_Q,
  NEON_UABAL    = NEON_SABAL | NEON3SameUBit,
  NEON_UABAL2   = NEON_UABAL | NEON_Q,
  NEON_UABDL    = NEON_SABDL | NEON3SameUBit,
  NEON_UABDL2   = NEON_UABDL | NEON_Q,
  NEON_UADDL    = NEON_SADDL | NEON3SameUBit,
  NEON_UADDL2   = NEON_UADDL | NEON_Q,
  NEON_UADDW    = NEON_SADDW | NEON3SameUBit,
  NEON_UADDW2   = NEON_UADDW | NEON_Q,
  NEON_UMLAL    = NEON_SMLAL | NEON3SameUBit,
  NEON_UMLAL2   = NEON_UMLAL | NEON_Q,
  NEON_UMLSL    = NEON_SMLSL | NEON3SameUBit,
  NEON_UMLSL2   = NEON_UMLSL | NEON_Q,
  NEON_UMULL    = NEON_SMULL | NEON3SameUBit,
  NEON_UMULL2   = NEON_UMULL | NEON_Q,
  NEON_USUBL    = NEON_SSUBL | NEON3SameUBit,
  NEON_USUBL2   = NEON_USUBL | NEON_Q,
  NEON_USUBW    = NEON_SSUBW | NEON3SameUBit,
  NEON_USUBW2   = NEON_USUBW | NEON_Q
};

// NEON instructions operating across vectors.
enum NEONAcrossLanesOp : uint32_t {
  NEONAcrossLanesFixed = 0x0E300800u,
  NEONAcrossLanesFMask = 0x9F3E0C00u,
  NEONAcrossLanesMask  = 0xBF3FFC00u,
  NEON_ADDV   = NEONAcrossLanesFixed | 0x0001B000u,
  NEON_SADDLV = NEONAcrossLanesFixed | 0x00003000u,
  NEON_UADDLV = NEONAcrossLanesFixed | 0x20003000u,
  NEON_SMAXV  = NEONAcrossLanesFixed | 0x0000A000u,
  NEON_SMINV  = NEONAcrossLanesFixed | 0x0001A000u,
  NEON_UMAXV  = NEONAcrossLanesFixed | 0x2000A000u,
  NEON_UMINV  = NEONAcrossLanesFixed | 0x2001A000u,

  NEONAcrossLanesFP16Fixed = NEONAcrossLanesFixed | 0x0000C000u,
  NEONAcrossLanesFP16FMask = NEONAcrossLanesFMask | 0x2000C000u,
  NEONAcrossLanesFP16Mask  = NEONAcrossLanesMask  | 0x20800000u,
  NEON_FMAXNMV_H = NEONAcrossLanesFP16Fixed | 0x00000000u,
  NEON_FMAXV_H   = NEONAcrossLanesFP16Fixed | 0x00003000u,
  NEON_FMINNMV_H = NEONAcrossLanesFP16Fixed | 0x00800000u,
  NEON_FMINV_H   = NEONAcrossLanesFP16Fixed | 0x00803000u,

  // NEON floating point across instructions.
  NEONAcrossLanesFPFixed = NEONAcrossLanesFixed | 0x2000C000u,
  NEONAcrossLanesFPFMask = NEONAcrossLanesFMask | 0x2000C000u,
  NEONAcrossLanesFPMask  = NEONAcrossLanesMask  | 0x20800000u,

  NEON_FMAXV   = NEONAcrossLanesFPFixed | 0x2000F000u,
  NEON_FMINV   = NEONAcrossLanesFPFixed | 0x2080F000u,
  NEON_FMAXNMV = NEONAcrossLanesFPFixed | 0x2000C000u,
  NEON_FMINNMV = NEONAcrossLanesFPFixed | 0x2080C000u
};

// NEON instructions with indexed element operand.
enum NEONByIndexedElementOp : uint32_t {
  NEONByIndexedElementFixed = 0x0F000000u,
  NEONByIndexedElementFMask = 0x9F000400u,
  NEONByIndexedElementMask  = 0xBF00F400u,
  NEON_MUL_byelement   = NEONByIndexedElementFixed | 0x00008000u,
  NEON_MLA_byelement   = NEONByIndexedElementFixed | 0x20000000u,
  NEON_MLS_byelement   = NEONByIndexedElementFixed | 0x20004000u,
  NEON_SMULL_byelement = NEONByIndexedElementFixed | 0x0000A000u,
  NEON_SMLAL_byelement = NEONByIndexedElementFixed | 0x00002000u,
  NEON_SMLSL_byelement = NEONByIndexedElementFixed | 0x00006000u,
  NEON_UMULL_byelement = NEONByIndexedElementFixed | 0x2000A000u,
  NEON_UMLAL_byelement = NEONByIndexedElementFixed | 0x20002000u,
  NEON_UMLSL_byelement = NEONByIndexedElementFixed | 0x20006000u,
  NEON_SQDMULL_byelement = NEONByIndexedElementFixed | 0x0000B000u,
  NEON_SQDMLAL_byelement = NEONByIndexedElementFixed | 0x00003000u,
  NEON_SQDMLSL_byelement = NEONByIndexedElementFixed | 0x00007000u,
  NEON_SQDMULH_byelement  = NEONByIndexedElementFixed | 0x0000C000u,
  NEON_SQRDMULH_byelement = NEONByIndexedElementFixed | 0x0000D000u,
  NEON_SDOT_byelement = NEONByIndexedElementFixed | 0x0000E000u,
  NEON_SQRDMLAH_byelement = NEONByIndexedElementFixed | 0x2000D000u,
  NEON_UDOT_byelement = NEONByIndexedElementFixed | 0x2000E000u,
  NEON_SQRDMLSH_byelement = NEONByIndexedElementFixed | 0x2000F000u,
  NEON_FMLA_H_byelement   = NEONByIndexedElementFixed | 0x00001000u,
  NEON_FMLS_H_byelement   = NEONByIndexedElementFixed | 0x00005000u,
  NEON_FMUL_H_byelement   = NEONByIndexedElementFixed | 0x00009000u,
  NEON_FMULX_H_byelement  = NEONByIndexedElementFixed | 0x20009000u,

  // Floating point instructions.
  NEONByIndexedElementFPFixed = NEONByIndexedElementFixed | 0x00800000u,
  NEONByIndexedElementFPMask = NEONByIndexedElementMask | 0x00800000u,
  NEON_FMLA_byelement  = NEONByIndexedElementFPFixed | 0x00001000u,
  NEON_FMLS_byelement  = NEONByIndexedElementFPFixed | 0x00005000u,
  NEON_FMUL_byelement  = NEONByIndexedElementFPFixed | 0x00009000u,
  NEON_FMULX_byelement = NEONByIndexedElementFPFixed | 0x20009000u,
  NEON_FCMLA_byelement = NEONByIndexedElementFixed | 0x20001000u,

  // Complex instruction(s) this is necessary because 'rot' encoding moves into the NEONByIndex..Mask space
  NEONByIndexedElementFPComplexMask = 0xBF009400u
};

// NEON register copy.
enum NEONCopyOp : uint32_t {
  NEONCopyFixed = 0x0E000400u,
  NEONCopyFMask = 0x9FE08400u,
  NEONCopyMask  = 0x3FE08400u,
  NEONCopyInsElementMask = NEONCopyMask | 0x40000000u,
  NEONCopyInsGeneralMask = NEONCopyMask | 0x40007800u,
  NEONCopyDupElementMask = NEONCopyMask | 0x20007800u,
  NEONCopyDupGeneralMask = NEONCopyDupElementMask,
  NEONCopyUmovMask       = NEONCopyMask | 0x20007800u,
  NEONCopySmovMask       = NEONCopyMask | 0x20007800u,
  NEON_INS_ELEMENT       = NEONCopyFixed | 0x60000000u,
  NEON_INS_GENERAL       = NEONCopyFixed | 0x40001800u,
  NEON_DUP_ELEMENT       = NEONCopyFixed | 0x00000000u,
  NEON_DUP_GENERAL       = NEONCopyFixed | 0x00000800u,
  NEON_SMOV              = NEONCopyFixed | 0x00002800u,
  NEON_UMOV              = NEONCopyFixed | 0x00003800u
};

// NEON extract.
enum NEONExtractOp : uint32_t {
  NEONExtractFixed = 0x2E000000u,
  NEONExtractFMask = 0xBF208400u,
  NEONExtractMask =  0xBFE08400u,
  NEON_EXT = NEONExtractFixed | 0x00000000u
};

enum NEONLoadStoreMultiOp : uint32_t {
  NEONLoadStoreMultiL    = 0x00400000u,
  NEONLoadStoreMulti1_1v = 0x00007000u,
  NEONLoadStoreMulti1_2v = 0x0000A000u,
  NEONLoadStoreMulti1_3v = 0x00006000u,
  NEONLoadStoreMulti1_4v = 0x00002000u,
  NEONLoadStoreMulti2    = 0x00008000u,
  NEONLoadStoreMulti3    = 0x00004000u,
  NEONLoadStoreMulti4    = 0x00000000u
};

// NEON load/store multiple structures.
enum NEONLoadStoreMultiStructOp : uint32_t {
  NEONLoadStoreMultiStructFixed = 0x0C000000u,
  NEONLoadStoreMultiStructFMask = 0xBFBF0000u,
  NEONLoadStoreMultiStructMask  = 0xBFFFF000u,
  NEONLoadStoreMultiStructStore = NEONLoadStoreMultiStructFixed,
  NEONLoadStoreMultiStructLoad  = NEONLoadStoreMultiStructFixed |
                                  NEONLoadStoreMultiL,
  NEON_LD1_1v = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti1_1v,
  NEON_LD1_2v = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti1_2v,
  NEON_LD1_3v = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti1_3v,
  NEON_LD1_4v = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti1_4v,
  NEON_LD2    = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti2,
  NEON_LD3    = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti3,
  NEON_LD4    = NEONLoadStoreMultiStructLoad | NEONLoadStoreMulti4,
  NEON_ST1_1v = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti1_1v,
  NEON_ST1_2v = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti1_2v,
  NEON_ST1_3v = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti1_3v,
  NEON_ST1_4v = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti1_4v,
  NEON_ST2    = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti2,
  NEON_ST3    = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti3,
  NEON_ST4    = NEONLoadStoreMultiStructStore | NEONLoadStoreMulti4
};

// NEON load/store multiple structures with post-index addressing.
enum NEONLoadStoreMultiStructPostIndexOp : uint32_t {
  NEONLoadStoreMultiStructPostIndexFixed = 0x0C800000u,
  NEONLoadStoreMultiStructPostIndexFMask = 0xBFA00000u,
  NEONLoadStoreMultiStructPostIndexMask  = 0xBFE0F000u,
  NEONLoadStoreMultiStructPostIndex = 0x00800000u,
  NEON_LD1_1v_post = NEON_LD1_1v | NEONLoadStoreMultiStructPostIndex,
  NEON_LD1_2v_post = NEON_LD1_2v | NEONLoadStoreMultiStructPostIndex,
  NEON_LD1_3v_post = NEON_LD1_3v | NEONLoadStoreMultiStructPostIndex,
  NEON_LD1_4v_post = NEON_LD1_4v | NEONLoadStoreMultiStructPostIndex,
  NEON_LD2_post = NEON_LD2 | NEONLoadStoreMultiStructPostIndex,
  NEON_LD3_post = NEON_LD3 | NEONLoadStoreMultiStructPostIndex,
  NEON_LD4_post = NEON_LD4 | NEONLoadStoreMultiStructPostIndex,
  NEON_ST1_1v_post = NEON_ST1_1v | NEONLoadStoreMultiStructPostIndex,
  NEON_ST1_2v_post = NEON_ST1_2v | NEONLoadStoreMultiStructPostIndex,
  NEON_ST1_3v_post = NEON_ST1_3v | NEONLoadStoreMultiStructPostIndex,
  NEON_ST1_4v_post = NEON_ST1_4v | NEONLoadStoreMultiStructPostIndex,
  NEON_ST2_post = NEON_ST2 | NEONLoadStoreMultiStructPostIndex,
  NEON_ST3_post = NEON_ST3 | NEONLoadStoreMultiStructPostIndex,
  NEON_ST4_post = NEON_ST4 | NEONLoadStoreMultiStructPostIndex
};

enum NEONLoadStoreSingleOp : uint32_t {
  NEONLoadStoreSingle1        = 0x00000000u,
  NEONLoadStoreSingle2        = 0x00200000u,
  NEONLoadStoreSingle3        = 0x00002000u,
  NEONLoadStoreSingle4        = 0x00202000u,
  NEONLoadStoreSingleL        = 0x00400000u,
  NEONLoadStoreSingle_b       = 0x00000000u,
  NEONLoadStoreSingle_h       = 0x00004000u,
  NEONLoadStoreSingle_s       = 0x00008000u,
  NEONLoadStoreSingle_d       = 0x00008400u,
  NEONLoadStoreSingleAllLanes = 0x0000C000u,
  NEONLoadStoreSingleLenMask  = 0x00202000u
};

// NEON load/store single structure.
enum NEONLoadStoreSingleStructOp : uint32_t {
  NEONLoadStoreSingleStructFixed = 0x0D000000u,
  NEONLoadStoreSingleStructFMask = 0xBF9F0000u,
  NEONLoadStoreSingleStructMask  = 0xBFFFE000u,
  NEONLoadStoreSingleStructStore = NEONLoadStoreSingleStructFixed,
  NEONLoadStoreSingleStructLoad  = NEONLoadStoreSingleStructFixed |
                                   NEONLoadStoreSingleL,
  NEONLoadStoreSingleStructLoad1 = NEONLoadStoreSingle1 |
                                   NEONLoadStoreSingleStructLoad,
  NEONLoadStoreSingleStructLoad2 = NEONLoadStoreSingle2 |
                                   NEONLoadStoreSingleStructLoad,
  NEONLoadStoreSingleStructLoad3 = NEONLoadStoreSingle3 |
                                   NEONLoadStoreSingleStructLoad,
  NEONLoadStoreSingleStructLoad4 = NEONLoadStoreSingle4 |
                                   NEONLoadStoreSingleStructLoad,
  NEONLoadStoreSingleStructStore1 = NEONLoadStoreSingle1 |
                                    NEONLoadStoreSingleStructFixed,
  NEONLoadStoreSingleStructStore2 = NEONLoadStoreSingle2 |
                                    NEONLoadStoreSingleStructFixed,
  NEONLoadStoreSingleStructStore3 = NEONLoadStoreSingle3 |
                                    NEONLoadStoreSingleStructFixed,
  NEONLoadStoreSingleStructStore4 = NEONLoadStoreSingle4 |
                                    NEONLoadStoreSingleStructFixed,
  NEON_LD1_b = NEONLoadStoreSingleStructLoad1 | NEONLoadStoreSingle_b,
  NEON_LD1_h = NEONLoadStoreSingleStructLoad1 | NEONLoadStoreSingle_h,
  NEON_LD1_s = NEONLoadStoreSingleStructLoad1 | NEONLoadStoreSingle_s,
  NEON_LD1_d = NEONLoadStoreSingleStructLoad1 | NEONLoadStoreSingle_d,
  NEON_LD1R  = NEONLoadStoreSingleStructLoad1 | NEONLoadStoreSingleAllLanes,
  NEON_ST1_b = NEONLoadStoreSingleStructStore1 | NEONLoadStoreSingle_b,
  NEON_ST1_h = NEONLoadStoreSingleStructStore1 | NEONLoadStoreSingle_h,
  NEON_ST1_s = NEONLoadStoreSingleStructStore1 | NEONLoadStoreSingle_s,
  NEON_ST1_d = NEONLoadStoreSingleStructStore1 | NEONLoadStoreSingle_d,

  NEON_LD2_b = NEONLoadStoreSingleStructLoad2 | NEONLoadStoreSingle_b,
  NEON_LD2_h = NEONLoadStoreSingleStructLoad2 | NEONLoadStoreSingle_h,
  NEON_LD2_s = NEONLoadStoreSingleStructLoad2 | NEONLoadStoreSingle_s,
  NEON_LD2_d = NEONLoadStoreSingleStructLoad2 | NEONLoadStoreSingle_d,
  NEON_LD2R  = NEONLoadStoreSingleStructLoad2 | NEONLoadStoreSingleAllLanes,
  NEON_ST2_b = NEONLoadStoreSingleStructStore2 | NEONLoadStoreSingle_b,
  NEON_ST2_h = NEONLoadStoreSingleStructStore2 | NEONLoadStoreSingle_h,
  NEON_ST2_s = NEONLoadStoreSingleStructStore2 | NEONLoadStoreSingle_s,
  NEON_ST2_d = NEONLoadStoreSingleStructStore2 | NEONLoadStoreSingle_d,

  NEON_LD3_b = NEONLoadStoreSingleStructLoad3 | NEONLoadStoreSingle_b,
  NEON_LD3_h = NEONLoadStoreSingleStructLoad3 | NEONLoadStoreSingle_h,
  NEON_LD3_s = NEONLoadStoreSingleStructLoad3 | NEONLoadStoreSingle_s,
  NEON_LD3_d = NEONLoadStoreSingleStructLoad3 | NEONLoadStoreSingle_d,
  NEON_LD3R  = NEONLoadStoreSingleStructLoad3 | NEONLoadStoreSingleAllLanes,
  NEON_ST3_b = NEONLoadStoreSingleStructStore3 | NEONLoadStoreSingle_b,
  NEON_ST3_h = NEONLoadStoreSingleStructStore3 | NEONLoadStoreSingle_h,
  NEON_ST3_s = NEONLoadStoreSingleStructStore3 | NEONLoadStoreSingle_s,
  NEON_ST3_d = NEONLoadStoreSingleStructStore3 | NEONLoadStoreSingle_d,

  NEON_LD4_b = NEONLoadStoreSingleStructLoad4 | NEONLoadStoreSingle_b,
  NEON_LD4_h = NEONLoadStoreSingleStructLoad4 | NEONLoadStoreSingle_h,
  NEON_LD4_s = NEONLoadStoreSingleStructLoad4 | NEONLoadStoreSingle_s,
  NEON_LD4_d = NEONLoadStoreSingleStructLoad4 | NEONLoadStoreSingle_d,
  NEON_LD4R  = NEONLoadStoreSingleStructLoad4 | NEONLoadStoreSingleAllLanes,
  NEON_ST4_b = NEONLoadStoreSingleStructStore4 | NEONLoadStoreSingle_b,
  NEON_ST4_h = NEONLoadStoreSingleStructStore4 | NEONLoadStoreSingle_h,
  NEON_ST4_s = NEONLoadStoreSingleStructStore4 | NEONLoadStoreSingle_s,
  NEON_ST4_d = NEONLoadStoreSingleStructStore4 | NEONLoadStoreSingle_d
};

// NEON load/store single structure with post-index addressing.
enum NEONLoadStoreSingleStructPostIndexOp : uint32_t {
  NEONLoadStoreSingleStructPostIndexFixed = 0x0D800000u,
  NEONLoadStoreSingleStructPostIndexFMask = 0xBF800000u,
  NEONLoadStoreSingleStructPostIndexMask  = 0xBFE0E000u,
  NEONLoadStoreSingleStructPostIndex =      0x00800000u,
  NEON_LD1_b_post = NEON_LD1_b | NEONLoadStoreSingleStructPostIndex,
  NEON_LD1_h_post = NEON_LD1_h | NEONLoadStoreSingleStructPostIndex,
  NEON_LD1_s_post = NEON_LD1_s | NEONLoadStoreSingleStructPostIndex,
  NEON_LD1_d_post = NEON_LD1_d | NEONLoadStoreSingleStructPostIndex,
  NEON_LD1R_post  = NEON_LD1R | NEONLoadStoreSingleStructPostIndex,
  NEON_ST1_b_post = NEON_ST1_b | NEONLoadStoreSingleStructPostIndex,
  NEON_ST1_h_post = NEON_ST1_h | NEONLoadStoreSingleStructPostIndex,
  NEON_ST1_s_post = NEON_ST1_s | NEONLoadStoreSingleStructPostIndex,
  NEON_ST1_d_post = NEON_ST1_d | NEONLoadStoreSingleStructPostIndex,

  NEON_LD2_b_post = NEON_LD2_b | NEONLoadStoreSingleStructPostIndex,
  NEON_LD2_h_post = NEON_LD2_h | NEONLoadStoreSingleStructPostIndex,
  NEON_LD2_s_post = NEON_LD2_s | NEONLoadStoreSingleStructPostIndex,
  NEON_LD2_d_post = NEON_LD2_d | NEONLoadStoreSingleStructPostIndex,
  NEON_LD2R_post  = NEON_LD2R | NEONLoadStoreSingleStructPostIndex,
  NEON_ST2_b_post = NEON_ST2_b | NEONLoadStoreSingleStructPostIndex,
  NEON_ST2_h_post = NEON_ST2_h | NEONLoadStoreSingleStructPostIndex,
  NEON_ST2_s_post = NEON_ST2_s | NEONLoadStoreSingleStructPostIndex,
  NEON_ST2_d_post = NEON_ST2_d | NEONLoadStoreSingleStructPostIndex,

  NEON_LD3_b_post = NEON_LD3_b | NEONLoadStoreSingleStructPostIndex,
  NEON_LD3_h_post = NEON_LD3_h | NEONLoadStoreSingleStructPostIndex,
  NEON_LD3_s_post = NEON_LD3_s | NEONLoadStoreSingleStructPostIndex,
  NEON_LD3_d_post = NEON_LD3_d | NEONLoadStoreSingleStructPostIndex,
  NEON_LD3R_post  = NEON_LD3R | NEONLoadStoreSingleStructPostIndex,
  NEON_ST3_b_post = NEON_ST3_b | NEONLoadStoreSingleStructPostIndex,
  NEON_ST3_h_post = NEON_ST3_h | NEONLoadStoreSingleStructPostIndex,
  NEON_ST3_s_post = NEON_ST3_s | NEONLoadStoreSingleStructPostIndex,
  NEON_ST3_d_post = NEON_ST3_d | NEONLoadStoreSingleStructPostIndex,

  NEON_LD4_b_post = NEON_LD4_b | NEONLoadStoreSingleStructPostIndex,
  NEON_LD4_h_post = NEON_LD4_h | NEONLoadStoreSingleStructPostIndex,
  NEON_LD4_s_post = NEON_LD4_s | NEONLoadStoreSingleStructPostIndex,
  NEON_LD4_d_post = NEON_LD4_d | NEONLoadStoreSingleStructPostIndex,
  NEON_LD4R_post  = NEON_LD4R | NEONLoadStoreSingleStructPostIndex,
  NEON_ST4_b_post = NEON_ST4_b | NEONLoadStoreSingleStructPostIndex,
  NEON_ST4_h_post = NEON_ST4_h | NEONLoadStoreSingleStructPostIndex,
  NEON_ST4_s_post = NEON_ST4_s | NEONLoadStoreSingleStructPostIndex,
  NEON_ST4_d_post = NEON_ST4_d | NEONLoadStoreSingleStructPostIndex
};

// NEON modified immediate.
enum NEONModifiedImmediateOp : uint32_t {
  NEONModifiedImmediateFixed = 0x0F000400u,
  NEONModifiedImmediateFMask = 0x9FF80400u,
  NEONModifiedImmediateOpBit = 0x20000000u,
  NEONModifiedImmediate_FMOV = NEONModifiedImmediateFixed | 0x00000800u,
  NEONModifiedImmediate_MOVI = NEONModifiedImmediateFixed | 0x00000000u,
  NEONModifiedImmediate_MVNI = NEONModifiedImmediateFixed | 0x20000000u,
  NEONModifiedImmediate_ORR  = NEONModifiedImmediateFixed | 0x00001000u,
  NEONModifiedImmediate_BIC  = NEONModifiedImmediateFixed | 0x20001000u
};

// NEON shift immediate.
enum NEONShiftImmediateOp : uint32_t {
  NEONShiftImmediateFixed = 0x0F000400u,
  NEONShiftImmediateFMask = 0x9F800400u,
  NEONShiftImmediateMask  = 0xBF80FC00u,
  NEONShiftImmediateUBit  = 0x20000000u,
  NEON_SHL      = NEONShiftImmediateFixed | 0x00005000u,
  NEON_SSHLL    = NEONShiftImmediateFixed | 0x0000A000u,
  NEON_USHLL    = NEONShiftImmediateFixed | 0x2000A000u,
  NEON_SLI      = NEONShiftImmediateFixed | 0x20005000u,
  NEON_SRI      = NEONShiftImmediateFixed | 0x20004000u,
  NEON_SHRN     = NEONShiftImmediateFixed | 0x00008000u,
  NEON_RSHRN    = NEONShiftImmediateFixed | 0x00008800u,
  NEON_UQSHRN   = NEONShiftImmediateFixed | 0x20009000u,
  NEON_UQRSHRN  = NEONShiftImmediateFixed | 0x20009800u,
  NEON_SQSHRN   = NEONShiftImmediateFixed | 0x00009000u,
  NEON_SQRSHRN  = NEONShiftImmediateFixed | 0x00009800u,
  NEON_SQSHRUN  = NEONShiftImmediateFixed | 0x20008000u,
  NEON_SQRSHRUN = NEONShiftImmediateFixed | 0x20008800u,
  NEON_SSHR     = NEONShiftImmediateFixed | 0x00000000u,
  NEON_SRSHR    = NEONShiftImmediateFixed | 0x00002000u,
  NEON_USHR     = NEONShiftImmediateFixed | 0x20000000u,
  NEON_URSHR    = NEONShiftImmediateFixed | 0x20002000u,
  NEON_SSRA     = NEONShiftImmediateFixed | 0x00001000u,
  NEON_SRSRA    = NEONShiftImmediateFixed | 0x00003000u,
  NEON_USRA     = NEONShiftImmediateFixed | 0x20001000u,
  NEON_URSRA    = NEONShiftImmediateFixed | 0x20003000u,
  NEON_SQSHLU   = NEONShiftImmediateFixed | 0x20006000u,
  NEON_SCVTF_imm = NEONShiftImmediateFixed | 0x0000E000u,
  NEON_UCVTF_imm = NEONShiftImmediateFixed | 0x2000E000u,
  NEON_FCVTZS_imm = NEONShiftImmediateFixed | 0x0000F800u,
  NEON_FCVTZU_imm = NEONShiftImmediateFixed | 0x2000F800u,
  NEON_SQSHL_imm = NEONShiftImmediateFixed | 0x00007000u,
  NEON_UQSHL_imm = NEONShiftImmediateFixed | 0x20007000u
};

// NEON table.
enum NEONTableOp : uint32_t {
  NEONTableFixed = 0x0E000000u,
  NEONTableFMask = 0xBF208C00u,
  NEONTableExt   = 0x00001000u,
  NEONTableMask  = 0xBF20FC00u,
  NEON_TBL_1v    = NEONTableFixed | 0x00000000u,
  NEON_TBL_2v    = NEONTableFixed | 0x00002000u,
  NEON_TBL_3v    = NEONTableFixed | 0x00004000u,
  NEON_TBL_4v    = NEONTableFixed | 0x00006000u,
  NEON_TBX_1v    = NEON_TBL_1v | NEONTableExt,
  NEON_TBX_2v    = NEON_TBL_2v | NEONTableExt,
  NEON_TBX_3v    = NEON_TBL_3v | NEONTableExt,
  NEON_TBX_4v    = NEON_TBL_4v | NEONTableExt
};

// NEON perm.
enum NEONPermOp : uint32_t {
  NEONPermFixed = 0x0E000800u,
  NEONPermFMask = 0xBF208C00u,
  NEONPermMask  = 0x3F20FC00u,
  NEON_UZP1 = NEONPermFixed | 0x00001000u,
  NEON_TRN1 = NEONPermFixed | 0x00002000u,
  NEON_ZIP1 = NEONPermFixed | 0x00003000u,
  NEON_UZP2 = NEONPermFixed | 0x00005000u,
  NEON_TRN2 = NEONPermFixed | 0x00006000u,
  NEON_ZIP2 = NEONPermFixed | 0x00007000u
};

// NEON scalar instructions with two register operands.
enum NEONScalar2RegMiscOp : uint32_t {
  NEONScalar2RegMiscFixed = 0x5E200800u,
  NEONScalar2RegMiscFMask = 0xDF3E0C00u,
  NEONScalar2RegMiscMask = NEON_Q | NEONScalar | NEON2RegMiscMask,
  NEON_CMGT_zero_scalar = NEON_Q | NEONScalar | NEON_CMGT_zero,
  NEON_CMEQ_zero_scalar = NEON_Q | NEONScalar | NEON_CMEQ_zero,
  NEON_CMLT_zero_scalar = NEON_Q | NEONScalar | NEON_CMLT_zero,
  NEON_CMGE_zero_scalar = NEON_Q | NEONScalar | NEON_CMGE_zero,
  NEON_CMLE_zero_scalar = NEON_Q | NEONScalar | NEON_CMLE_zero,
  NEON_ABS_scalar       = NEON_Q | NEONScalar | NEON_ABS,
  NEON_SQABS_scalar     = NEON_Q | NEONScalar | NEON_SQABS,
  NEON_NEG_scalar       = NEON_Q | NEONScalar | NEON_NEG,
  NEON_SQNEG_scalar     = NEON_Q | NEONScalar | NEON_SQNEG,
  NEON_SQXTN_scalar     = NEON_Q | NEONScalar | NEON_SQXTN,
  NEON_UQXTN_scalar     = NEON_Q | NEONScalar | NEON_UQXTN,
  NEON_SQXTUN_scalar    = NEON_Q | NEONScalar | NEON_SQXTUN,
  NEON_SUQADD_scalar    = NEON_Q | NEONScalar | NEON_SUQADD,
  NEON_USQADD_scalar    = NEON_Q | NEONScalar | NEON_USQADD,

  NEONScalar2RegMiscOpcode = NEON2RegMiscOpcode,
  NEON_NEG_scalar_opcode = NEON_NEG_scalar & NEONScalar2RegMiscOpcode,

  NEONScalar2RegMiscFPMask  = NEONScalar2RegMiscMask | 0x00800000,
  NEON_FRSQRTE_scalar    = NEON_Q | NEONScalar | NEON_FRSQRTE,
  NEON_FRECPE_scalar     = NEON_Q | NEONScalar | NEON_FRECPE,
  NEON_SCVTF_scalar      = NEON_Q | NEONScalar | NEON_SCVTF,
  NEON_UCVTF_scalar      = NEON_Q | NEONScalar | NEON_UCVTF,
  NEON_FCMGT_zero_scalar = NEON_Q | NEONScalar | NEON_FCMGT_zero,
  NEON_FCMEQ_zero_scalar = NEON_Q | NEONScalar | NEON_FCMEQ_zero,
  NEON_FCMLT_zero_scalar = NEON_Q | NEONScalar | NEON_FCMLT_zero,
  NEON_FCMGE_zero_scalar = NEON_Q | NEONScalar | NEON_FCMGE_zero,
  NEON_FCMLE_zero_scalar = NEON_Q | NEONScalar | NEON_FCMLE_zero,
  NEON_FRECPX_scalar     = NEONScalar2RegMiscFixed | 0x0081F000,
  NEON_FCVTNS_scalar     = NEON_Q | NEONScalar | NEON_FCVTNS,
  NEON_FCVTNU_scalar     = NEON_Q | NEONScalar | NEON_FCVTNU,
  NEON_FCVTPS_scalar     = NEON_Q | NEONScalar | NEON_FCVTPS,
  NEON_FCVTPU_scalar     = NEON_Q | NEONScalar | NEON_FCVTPU,
  NEON_FCVTMS_scalar     = NEON_Q | NEONScalar | NEON_FCVTMS,
  NEON_FCVTMU_scalar     = NEON_Q | NEONScalar | NEON_FCVTMU,
  NEON_FCVTZS_scalar     = NEON_Q | NEONScalar | NEON_FCVTZS,
  NEON_FCVTZU_scalar     = NEON_Q | NEONScalar | NEON_FCVTZU,
  NEON_FCVTAS_scalar     = NEON_Q | NEONScalar | NEON_FCVTAS,
  NEON_FCVTAU_scalar     = NEON_Q | NEONScalar | NEON_FCVTAU,
  NEON_FCVTXN_scalar     = NEON_Q | NEONScalar | NEON_FCVTXN
};

// NEON instructions with two register operands (FP16).
enum NEONScalar2RegMiscFP16Op : uint32_t {
  NEONScalar2RegMiscFP16Fixed = 0x5E780800u,
  NEONScalar2RegMiscFP16FMask = 0xDF7E0C00u,
  NEONScalar2RegMiscFP16Mask  = 0xFFFFFC00u,
  NEON_FCVTNS_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTNS_H,
  NEON_FCVTMS_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTMS_H,
  NEON_FCVTAS_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTAS_H,
  NEON_SCVTF_H_scalar      = NEON_Q | NEONScalar | NEON_SCVTF_H,
  NEON_FCMGT_H_zero_scalar = NEON_Q | NEONScalar | NEON_FCMGT_H_zero,
  NEON_FCMEQ_H_zero_scalar = NEON_Q | NEONScalar | NEON_FCMEQ_H_zero,
  NEON_FCMLT_H_zero_scalar = NEON_Q | NEONScalar | NEON_FCMLT_H_zero,
  NEON_FCVTPS_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTPS_H,
  NEON_FCVTZS_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTZS_H,
  NEON_FRECPE_H_scalar     = NEON_Q | NEONScalar | NEON_FRECPE_H,
  NEON_FRECPX_H_scalar     = NEONScalar2RegMiscFP16Fixed | 0x0081F000u,
  NEON_FCVTNU_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTNU_H,
  NEON_FCVTMU_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTMU_H,
  NEON_FCVTAU_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTAU_H,
  NEON_UCVTF_H_scalar      = NEON_Q | NEONScalar | NEON_UCVTF_H,
  NEON_FCMGE_H_zero_scalar = NEON_Q | NEONScalar | NEON_FCMGE_H_zero,
  NEON_FCMLE_H_zero_scalar = NEON_Q | NEONScalar | NEON_FCMLE_H_zero,
  NEON_FCVTPU_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTPU_H,
  NEON_FCVTZU_H_scalar     = NEON_Q | NEONScalar | NEON_FCVTZU_H,
  NEON_FRSQRTE_H_scalar    = NEON_Q | NEONScalar | NEON_FRSQRTE_H
};

// NEON scalar instructions with three same-type operands.
enum NEONScalar3SameOp : uint32_t {
  NEONScalar3SameFixed = 0x5E200400u,
  NEONScalar3SameFMask = 0xDF200400u,
  NEONScalar3SameMask  = 0xFF20FC00u,
  NEON_ADD_scalar    = NEON_Q | NEONScalar | NEON_ADD,
  NEON_CMEQ_scalar   = NEON_Q | NEONScalar | NEON_CMEQ,
  NEON_CMGE_scalar   = NEON_Q | NEONScalar | NEON_CMGE,
  NEON_CMGT_scalar   = NEON_Q | NEONScalar | NEON_CMGT,
  NEON_CMHI_scalar   = NEON_Q | NEONScalar | NEON_CMHI,
  NEON_CMHS_scalar   = NEON_Q | NEONScalar | NEON_CMHS,
  NEON_CMTST_scalar  = NEON_Q | NEONScalar | NEON_CMTST,
  NEON_SUB_scalar    = NEON_Q | NEONScalar | NEON_SUB,
  NEON_UQADD_scalar  = NEON_Q | NEONScalar | NEON_UQADD,
  NEON_SQADD_scalar  = NEON_Q | NEONScalar | NEON_SQADD,
  NEON_UQSUB_scalar  = NEON_Q | NEONScalar | NEON_UQSUB,
  NEON_SQSUB_scalar  = NEON_Q | NEONScalar | NEON_SQSUB,
  NEON_USHL_scalar   = NEON_Q | NEONScalar | NEON_USHL,
  NEON_SSHL_scalar   = NEON_Q | NEONScalar | NEON_SSHL,
  NEON_UQSHL_scalar  = NEON_Q | NEONScalar | NEON_UQSHL,
  NEON_SQSHL_scalar  = NEON_Q | NEONScalar | NEON_SQSHL,
  NEON_URSHL_scalar  = NEON_Q | NEONScalar | NEON_URSHL,
  NEON_SRSHL_scalar  = NEON_Q | NEONScalar | NEON_SRSHL,
  NEON_UQRSHL_scalar = NEON_Q | NEONScalar | NEON_UQRSHL,
  NEON_SQRSHL_scalar = NEON_Q | NEONScalar | NEON_SQRSHL,
  NEON_SQDMULH_scalar = NEON_Q | NEONScalar | NEON_SQDMULH,
  NEON_SQRDMULH_scalar = NEON_Q | NEONScalar | NEON_SQRDMULH,

  // NEON floating point scalar instructions with three same-type operands.
  NEONScalar3SameFPFixed = NEONScalar3SameFixed | 0x0000C000u,
  NEONScalar3SameFPFMask = NEONScalar3SameFMask | 0x0000C000u,
  NEONScalar3SameFPMask  = NEONScalar3SameMask | 0x00800000u,
  NEON_FACGE_scalar   = NEON_Q | NEONScalar | NEON_FACGE,
  NEON_FACGT_scalar   = NEON_Q | NEONScalar | NEON_FACGT,
  NEON_FCMEQ_scalar   = NEON_Q | NEONScalar | NEON_FCMEQ,
  NEON_FCMGE_scalar   = NEON_Q | NEONScalar | NEON_FCMGE,
  NEON_FCMGT_scalar   = NEON_Q | NEONScalar | NEON_FCMGT,
  NEON_FMULX_scalar   = NEON_Q | NEONScalar | NEON_FMULX,
  NEON_FRECPS_scalar  = NEON_Q | NEONScalar | NEON_FRECPS,
  NEON_FRSQRTS_scalar = NEON_Q | NEONScalar | NEON_FRSQRTS,
  NEON_FABD_scalar    = NEON_Q | NEONScalar | NEON_FABD
};

// NEON scalar FP16 instructions with three same-type operands.
enum NEONScalar3SameFP16Op : uint32_t {
  NEONScalar3SameFP16Fixed = 0x5E400400u,
  NEONScalar3SameFP16FMask = 0xDF60C400u,
  NEONScalar3SameFP16Mask  = 0xFFE0FC00u,
  NEON_FABD_H_scalar    = NEON_Q | NEONScalar | NEON_FABD_H,
  NEON_FMULX_H_scalar   = NEON_Q | NEONScalar | NEON_FMULX_H,
  NEON_FCMEQ_H_scalar   = NEON_Q | NEONScalar | NEON_FCMEQ_H,
  NEON_FCMGE_H_scalar   = NEON_Q | NEONScalar | NEON_FCMGE_H,
  NEON_FCMGT_H_scalar   = NEON_Q | NEONScalar | NEON_FCMGT_H,
  NEON_FACGE_H_scalar   = NEON_Q | NEONScalar | NEON_FACGE_H,
  NEON_FACGT_H_scalar   = NEON_Q | NEONScalar | NEON_FACGT_H,
  NEON_FRECPS_H_scalar  = NEON_Q | NEONScalar | NEON_FRECPS_H,
  NEON_FRSQRTS_H_scalar = NEON_Q | NEONScalar | NEON_FRSQRTS_H
};

// 'Extra' NEON scalar instructions with three same-type operands.
enum NEONScalar3SameExtraOp : uint32_t {
  NEONScalar3SameExtraFixed = 0x5E008400u,
  NEONScalar3SameExtraFMask = 0xDF208400u,
  NEONScalar3SameExtraMask = 0xFF20FC00u,
  NEON_SQRDMLAH_scalar = NEON_Q | NEONScalar | NEON_SQRDMLAH,
  NEON_SQRDMLSH_scalar = NEON_Q | NEONScalar | NEON_SQRDMLSH
};

// NEON scalar instructions with three different-type operands.
enum NEONScalar3DiffOp : uint32_t {
  NEONScalar3DiffFixed = 0x5E200000u,
  NEONScalar3DiffFMask = 0xDF200C00u,
  NEONScalar3DiffMask  = NEON_Q | NEONScalar | NEON3DifferentMask,
  NEON_SQDMLAL_scalar  = NEON_Q | NEONScalar | NEON_SQDMLAL,
  NEON_SQDMLSL_scalar  = NEON_Q | NEONScalar | NEON_SQDMLSL,
  NEON_SQDMULL_scalar  = NEON_Q | NEONScalar | NEON_SQDMULL
};

// NEON scalar instructions with indexed element operand.
enum NEONScalarByIndexedElementOp : uint32_t {
  NEONScalarByIndexedElementFixed = 0x5F000000u,
  NEONScalarByIndexedElementFMask = 0xDF000400u,
  NEONScalarByIndexedElementMask  = 0xFF00F400u,
  NEON_SQDMLAL_byelement_scalar  = NEON_Q | NEONScalar | NEON_SQDMLAL_byelement,
  NEON_SQDMLSL_byelement_scalar  = NEON_Q | NEONScalar | NEON_SQDMLSL_byelement,
  NEON_SQDMULL_byelement_scalar  = NEON_Q | NEONScalar | NEON_SQDMULL_byelement,
  NEON_SQDMULH_byelement_scalar  = NEON_Q | NEONScalar | NEON_SQDMULH_byelement,
  NEON_SQRDMULH_byelement_scalar
    = NEON_Q | NEONScalar | NEON_SQRDMULH_byelement,
  NEON_SQRDMLAH_byelement_scalar
    = NEON_Q | NEONScalar | NEON_SQRDMLAH_byelement,
  NEON_SQRDMLSH_byelement_scalar
    = NEON_Q | NEONScalar | NEON_SQRDMLSH_byelement,
  NEON_FMLA_H_byelement_scalar  = NEON_Q | NEONScalar | NEON_FMLA_H_byelement,
  NEON_FMLS_H_byelement_scalar  = NEON_Q | NEONScalar | NEON_FMLS_H_byelement,
  NEON_FMUL_H_byelement_scalar  = NEON_Q | NEONScalar | NEON_FMUL_H_byelement,
  NEON_FMULX_H_byelement_scalar = NEON_Q | NEONScalar | NEON_FMULX_H_byelement,

  // Floating point instructions.
  NEONScalarByIndexedElementFPFixed
    = NEONScalarByIndexedElementFixed | 0x00800000u,
  NEONScalarByIndexedElementFPMask
    = NEONScalarByIndexedElementMask | 0x00800000u,
  NEON_FMLA_byelement_scalar  = NEON_Q | NEONScalar | NEON_FMLA_byelement,
  NEON_FMLS_byelement_scalar  = NEON_Q | NEONScalar | NEON_FMLS_byelement,
  NEON_FMUL_byelement_scalar  = NEON_Q | NEONScalar | NEON_FMUL_byelement,
  NEON_FMULX_byelement_scalar = NEON_Q | NEONScalar | NEON_FMULX_byelement
};

// NEON scalar register copy.
enum NEONScalarCopyOp : uint32_t {
  NEONScalarCopyFixed = 0x5E000400u,
  NEONScalarCopyFMask = 0xDFE08400u,
  NEONScalarCopyMask  = 0xFFE0FC00u,
  NEON_DUP_ELEMENT_scalar = NEON_Q | NEONScalar | NEON_DUP_ELEMENT
};

// NEON scalar pairwise instructions.
enum NEONScalarPairwiseOp : uint32_t {
  NEONScalarPairwiseFixed = 0x5E300800u,
  NEONScalarPairwiseFMask = 0xDF3E0C00u,
  NEONScalarPairwiseMask  = 0xFFB1F800u,
  NEON_ADDP_scalar      = NEONScalarPairwiseFixed | 0x0081B000u,
  NEON_FMAXNMP_h_scalar = NEONScalarPairwiseFixed | 0x0000C000u,
  NEON_FADDP_h_scalar   = NEONScalarPairwiseFixed | 0x0000D000u,
  NEON_FMAXP_h_scalar   = NEONScalarPairwiseFixed | 0x0000F000u,
  NEON_FMINNMP_h_scalar = NEONScalarPairwiseFixed | 0x0080C000u,
  NEON_FMINP_h_scalar   = NEONScalarPairwiseFixed | 0x0080F000u,
  NEON_FMAXNMP_scalar   = NEONScalarPairwiseFixed | 0x2000C000u,
  NEON_FMINNMP_scalar   = NEONScalarPairwiseFixed | 0x2080C000u,
  NEON_FADDP_scalar     = NEONScalarPairwiseFixed | 0x2000D000u,
  NEON_FMAXP_scalar     = NEONScalarPairwiseFixed | 0x2000F000u,
  NEON_FMINP_scalar     = NEONScalarPairwiseFixed | 0x2080F000u
};

// NEON scalar shift immediate.
enum NEONScalarShiftImmediateOp : uint32_t {
  NEONScalarShiftImmediateFixed = 0x5F000400u,
  NEONScalarShiftImmediateFMask = 0xDF800400u,
  NEONScalarShiftImmediateMask  = 0xFF80FC00u,
  NEON_SHL_scalar  =       NEON_Q | NEONScalar | NEON_SHL,
  NEON_SLI_scalar  =       NEON_Q | NEONScalar | NEON_SLI,
  NEON_SRI_scalar  =       NEON_Q | NEONScalar | NEON_SRI,
  NEON_SSHR_scalar =       NEON_Q | NEONScalar | NEON_SSHR,
  NEON_USHR_scalar =       NEON_Q | NEONScalar | NEON_USHR,
  NEON_SRSHR_scalar =      NEON_Q | NEONScalar | NEON_SRSHR,
  NEON_URSHR_scalar =      NEON_Q | NEONScalar | NEON_URSHR,
  NEON_SSRA_scalar =       NEON_Q | NEONScalar | NEON_SSRA,
  NEON_USRA_scalar =       NEON_Q | NEONScalar | NEON_USRA,
  NEON_SRSRA_scalar =      NEON_Q | NEONScalar | NEON_SRSRA,
  NEON_URSRA_scalar =      NEON_Q | NEONScalar | NEON_URSRA,
  NEON_UQSHRN_scalar =     NEON_Q | NEONScalar | NEON_UQSHRN,
  NEON_UQRSHRN_scalar =    NEON_Q | NEONScalar | NEON_UQRSHRN,
  NEON_SQSHRN_scalar =     NEON_Q | NEONScalar | NEON_SQSHRN,
  NEON_SQRSHRN_scalar =    NEON_Q | NEONScalar | NEON_SQRSHRN,
  NEON_SQSHRUN_scalar =    NEON_Q | NEONScalar | NEON_SQSHRUN,
  NEON_SQRSHRUN_scalar =   NEON_Q | NEONScalar | NEON_SQRSHRUN,
  NEON_SQSHLU_scalar =     NEON_Q | NEONScalar | NEON_SQSHLU,
  NEON_SQSHL_imm_scalar  = NEON_Q | NEONScalar | NEON_SQSHL_imm,
  NEON_UQSHL_imm_scalar  = NEON_Q | NEONScalar | NEON_UQSHL_imm,
  NEON_SCVTF_imm_scalar =  NEON_Q | NEONScalar | NEON_SCVTF_imm,
  NEON_UCVTF_imm_scalar =  NEON_Q | NEONScalar | NEON_UCVTF_imm,
  NEON_FCVTZS_imm_scalar = NEON_Q | NEONScalar | NEON_FCVTZS_imm,
  NEON_FCVTZU_imm_scalar = NEON_Q | NEONScalar | NEON_FCVTZU_imm
};

// Unimplemented and unallocated instructions. These are defined to make fixed
// bit assertion easier.
enum UnimplementedOp : uint32_t {
  UnimplementedFixed = 0x00000000u,
  UnimplementedFMask = 0x00000000u
};

enum UnallocatedOp : uint32_t {
  UnallocatedFixed = 0x00000000u,
  UnallocatedFMask = 0x00000000u
};

// Re-enable `clang-format` after the `enum`s.
// clang-format on

}  // namespace aarch64
}  // namespace vixl

#endif  // VIXL_AARCH64_CONSTANTS_AARCH64_H_
