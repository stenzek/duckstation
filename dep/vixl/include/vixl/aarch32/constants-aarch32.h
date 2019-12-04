// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may
//     be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_CONSTANTS_AARCH32_H_
#define VIXL_CONSTANTS_AARCH32_H_

extern "C" {
#include <stdint.h>
}

#include "globals-vixl.h"


namespace vixl {
namespace aarch32 {

enum InstructionSet { A32, T32 };
#ifdef VIXL_INCLUDE_TARGET_T32_ONLY
const InstructionSet kDefaultISA = T32;
#else
const InstructionSet kDefaultISA = A32;
#endif

const unsigned kRegSizeInBits = 32;
const unsigned kRegSizeInBytes = kRegSizeInBits / 8;
const unsigned kSRegSizeInBits = 32;
const unsigned kSRegSizeInBytes = kSRegSizeInBits / 8;
const unsigned kDRegSizeInBits = 64;
const unsigned kDRegSizeInBytes = kDRegSizeInBits / 8;
const unsigned kQRegSizeInBits = 128;
const unsigned kQRegSizeInBytes = kQRegSizeInBits / 8;

const unsigned kNumberOfRegisters = 16;
const unsigned kNumberOfSRegisters = 32;
const unsigned kMaxNumberOfDRegisters = 32;
const unsigned kNumberOfQRegisters = 16;
const unsigned kNumberOfT32LowRegisters = 8;

const unsigned kIpCode = 12;
const unsigned kSpCode = 13;
const unsigned kLrCode = 14;
const unsigned kPcCode = 15;

const unsigned kT32PcDelta = 4;
const unsigned kA32PcDelta = 8;

const unsigned kRRXEncodedValue = 3;

const unsigned kCoprocMask = 0xe;
const unsigned kInvalidCoprocMask = 0xa;

const unsigned kLowestT32_32Opcode = 0xe8000000;

const uint32_t kUnknownValue = 0xdeadbeef;

const uint32_t kMaxInstructionSizeInBytes = 4;
const uint32_t kA32InstructionSizeInBytes = 4;
const uint32_t k32BitT32InstructionSizeInBytes = 4;
const uint32_t k16BitT32InstructionSizeInBytes = 2;

// Maximum size emitted by a single T32 unconditional macro-instruction.
const uint32_t kMaxT32MacroInstructionSizeInBytes = 32;

const uint32_t kCallerSavedRegistersMask = 0x500f;

const uint16_t k16BitT32NopOpcode = 0xbf00;
const uint16_t kCbzCbnzMask = 0xf500;
const uint16_t kCbzCbnzValue = 0xb100;

const int32_t kCbzCbnzRange = 126;
const int32_t kBConditionalNarrowRange = 254;
const int32_t kBNarrowRange = 2046;
const int32_t kNearLabelRange = kBNarrowRange;

enum SystemFunctionsOpcodes { kPrintfCode };

enum BranchHint { kNear, kFar, kBranchWithoutHint };

// Start of generated code.
// AArch32 version implemented by the library (v8.0).
// The encoding for vX.Y is: (X << 8) | Y.
#define AARCH32_VERSION 0x0800

enum InstructionAttribute {
  kNoAttribute = 0,
  kArithmetic = 0x1,
  kBitwise = 0x2,
  kShift = 0x4,
  kAddress = 0x8,
  kBranch = 0x10,
  kSystem = 0x20,
  kFpNeon = 0x40,
  kLoadStore = 0x80,
  kLoadStoreMultiple = 0x100
};

enum InstructionType {
  kUndefInstructionType,
  kAdc,
  kAdcs,
  kAdd,
  kAdds,
  kAddw,
  kAdr,
  kAnd,
  kAnds,
  kAsr,
  kAsrs,
  kB,
  kBfc,
  kBfi,
  kBic,
  kBics,
  kBkpt,
  kBl,
  kBlx,
  kBx,
  kBxj,
  kCbnz,
  kCbz,
  kClrex,
  kClz,
  kCmn,
  kCmp,
  kCrc32b,
  kCrc32cb,
  kCrc32ch,
  kCrc32cw,
  kCrc32h,
  kCrc32w,
  kDmb,
  kDsb,
  kEor,
  kEors,
  kFldmdbx,
  kFldmiax,
  kFstmdbx,
  kFstmiax,
  kHlt,
  kHvc,
  kIsb,
  kIt,
  kLda,
  kLdab,
  kLdaex,
  kLdaexb,
  kLdaexd,
  kLdaexh,
  kLdah,
  kLdm,
  kLdmda,
  kLdmdb,
  kLdmea,
  kLdmed,
  kLdmfa,
  kLdmfd,
  kLdmib,
  kLdr,
  kLdrb,
  kLdrd,
  kLdrex,
  kLdrexb,
  kLdrexd,
  kLdrexh,
  kLdrh,
  kLdrsb,
  kLdrsh,
  kLsl,
  kLsls,
  kLsr,
  kLsrs,
  kMla,
  kMlas,
  kMls,
  kMov,
  kMovs,
  kMovt,
  kMovw,
  kMrs,
  kMsr,
  kMul,
  kMuls,
  kMvn,
  kMvns,
  kNop,
  kOrn,
  kOrns,
  kOrr,
  kOrrs,
  kPkhbt,
  kPkhtb,
  kPld,
  kPldw,
  kPli,
  kPop,
  kPush,
  kQadd,
  kQadd16,
  kQadd8,
  kQasx,
  kQdadd,
  kQdsub,
  kQsax,
  kQsub,
  kQsub16,
  kQsub8,
  kRbit,
  kRev,
  kRev16,
  kRevsh,
  kRor,
  kRors,
  kRrx,
  kRrxs,
  kRsb,
  kRsbs,
  kRsc,
  kRscs,
  kSadd16,
  kSadd8,
  kSasx,
  kSbc,
  kSbcs,
  kSbfx,
  kSdiv,
  kSel,
  kShadd16,
  kShadd8,
  kShasx,
  kShsax,
  kShsub16,
  kShsub8,
  kSmlabb,
  kSmlabt,
  kSmlad,
  kSmladx,
  kSmlal,
  kSmlalbb,
  kSmlalbt,
  kSmlald,
  kSmlaldx,
  kSmlals,
  kSmlaltb,
  kSmlaltt,
  kSmlatb,
  kSmlatt,
  kSmlawb,
  kSmlawt,
  kSmlsd,
  kSmlsdx,
  kSmlsld,
  kSmlsldx,
  kSmmla,
  kSmmlar,
  kSmmls,
  kSmmlsr,
  kSmmul,
  kSmmulr,
  kSmuad,
  kSmuadx,
  kSmulbb,
  kSmulbt,
  kSmull,
  kSmulls,
  kSmultb,
  kSmultt,
  kSmulwb,
  kSmulwt,
  kSmusd,
  kSmusdx,
  kSsat,
  kSsat16,
  kSsax,
  kSsub16,
  kSsub8,
  kStl,
  kStlb,
  kStlex,
  kStlexb,
  kStlexd,
  kStlexh,
  kStlh,
  kStm,
  kStmda,
  kStmdb,
  kStmea,
  kStmed,
  kStmfa,
  kStmfd,
  kStmib,
  kStr,
  kStrb,
  kStrd,
  kStrex,
  kStrexb,
  kStrexd,
  kStrexh,
  kStrh,
  kSub,
  kSubs,
  kSubw,
  kSvc,
  kSxtab,
  kSxtab16,
  kSxtah,
  kSxtb,
  kSxtb16,
  kSxth,
  kTbb,
  kTbh,
  kTeq,
  kTst,
  kUadd16,
  kUadd8,
  kUasx,
  kUbfx,
  kUdf,
  kUdiv,
  kUhadd16,
  kUhadd8,
  kUhasx,
  kUhsax,
  kUhsub16,
  kUhsub8,
  kUmaal,
  kUmlal,
  kUmlals,
  kUmull,
  kUmulls,
  kUqadd16,
  kUqadd8,
  kUqasx,
  kUqsax,
  kUqsub16,
  kUqsub8,
  kUsad8,
  kUsada8,
  kUsat,
  kUsat16,
  kUsax,
  kUsub16,
  kUsub8,
  kUxtab,
  kUxtab16,
  kUxtah,
  kUxtb,
  kUxtb16,
  kUxth,
  kVaba,
  kVabal,
  kVabd,
  kVabdl,
  kVabs,
  kVacge,
  kVacgt,
  kVacle,
  kVaclt,
  kVadd,
  kVaddhn,
  kVaddl,
  kVaddw,
  kVand,
  kVbic,
  kVbif,
  kVbit,
  kVbsl,
  kVceq,
  kVcge,
  kVcgt,
  kVcle,
  kVcls,
  kVclt,
  kVclz,
  kVcmp,
  kVcmpe,
  kVcnt,
  kVcvt,
  kVcvta,
  kVcvtb,
  kVcvtm,
  kVcvtn,
  kVcvtp,
  kVcvtr,
  kVcvtt,
  kVdiv,
  kVdup,
  kVeor,
  kVext,
  kVfma,
  kVfms,
  kVfnma,
  kVfnms,
  kVhadd,
  kVhsub,
  kVld1,
  kVld2,
  kVld3,
  kVld4,
  kVldm,
  kVldmdb,
  kVldmia,
  kVldr,
  kVmax,
  kVmaxnm,
  kVmin,
  kVminnm,
  kVmla,
  kVmlal,
  kVmls,
  kVmlsl,
  kVmov,
  kVmovl,
  kVmovn,
  kVmrs,
  kVmsr,
  kVmul,
  kVmull,
  kVmvn,
  kVneg,
  kVnmla,
  kVnmls,
  kVnmul,
  kVorn,
  kVorr,
  kVpadal,
  kVpadd,
  kVpaddl,
  kVpmax,
  kVpmin,
  kVpop,
  kVpush,
  kVqabs,
  kVqadd,
  kVqdmlal,
  kVqdmlsl,
  kVqdmulh,
  kVqdmull,
  kVqmovn,
  kVqmovun,
  kVqneg,
  kVqrdmulh,
  kVqrshl,
  kVqrshrn,
  kVqrshrun,
  kVqshl,
  kVqshlu,
  kVqshrn,
  kVqshrun,
  kVqsub,
  kVraddhn,
  kVrecpe,
  kVrecps,
  kVrev16,
  kVrev32,
  kVrev64,
  kVrhadd,
  kVrinta,
  kVrintm,
  kVrintn,
  kVrintp,
  kVrintr,
  kVrintx,
  kVrintz,
  kVrshl,
  kVrshr,
  kVrshrn,
  kVrsqrte,
  kVrsqrts,
  kVrsra,
  kVrsubhn,
  kVseleq,
  kVselge,
  kVselgt,
  kVselvs,
  kVshl,
  kVshll,
  kVshr,
  kVshrn,
  kVsli,
  kVsqrt,
  kVsra,
  kVsri,
  kVst1,
  kVst2,
  kVst3,
  kVst4,
  kVstm,
  kVstmdb,
  kVstmia,
  kVstr,
  kVsub,
  kVsubhn,
  kVsubl,
  kVsubw,
  kVswp,
  kVtbl,
  kVtbx,
  kVtrn,
  kVtst,
  kVuzp,
  kVzip,
  kYield
};

const char* ToCString(InstructionType type);
// End of generated code.

inline InstructionAttribute operator|(InstructionAttribute left,
                                      InstructionAttribute right) {
  return static_cast<InstructionAttribute>(static_cast<uint32_t>(left) |
                                           static_cast<uint32_t>(right));
}

}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_CONSTANTS_AARCH32_H_
