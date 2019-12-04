// Copyright 2017, VIXL authors
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

#ifndef VIXL_AARCH32_ASSEMBLER_AARCH32_H_
#define VIXL_AARCH32_ASSEMBLER_AARCH32_H_

#include "assembler-base-vixl.h"

#include "aarch32/instructions-aarch32.h"
#include "aarch32/location-aarch32.h"

namespace vixl {
namespace aarch32 {

class Assembler : public internal::AssemblerBase {
  InstructionSet isa_;
  Condition first_condition_;
  uint16_t it_mask_;
  bool has_32_dregs_;
  bool allow_unpredictable_;
  bool allow_strongly_discouraged_;

 protected:
  void EmitT32_16(uint16_t instr);
  void EmitT32_32(uint32_t instr);
  void EmitA32(uint32_t instr);
  // Check that the condition of the current instruction is consistent with the
  // IT state.
  void CheckIT(Condition condition) {
#ifdef VIXL_DEBUG
    PerformCheckIT(condition);
#else
    USE(condition);
#endif
  }
#ifdef VIXL_DEBUG
  void PerformCheckIT(Condition condition);
#endif
  void AdvanceIT() {
    first_condition_ =
        Condition((first_condition_.GetCondition() & 0xe) | (it_mask_ >> 3));
    it_mask_ = (it_mask_ << 1) & 0xf;
  }
  // Virtual, in order to be overridden by the MacroAssembler, which needs to
  // notify the pool manager.
  virtual void BindHelper(Label* label);

  uint32_t Link(uint32_t instr,
                Location* location,
                const Location::EmitOperator& op,
                const ReferenceInfo* info);

 public:
  class AllowUnpredictableScope {
    Assembler* assembler_;
    bool old_;

   public:
    explicit AllowUnpredictableScope(Assembler* assembler)
        : assembler_(assembler), old_(assembler->allow_unpredictable_) {
      assembler_->allow_unpredictable_ = true;
    }
    ~AllowUnpredictableScope() { assembler_->allow_unpredictable_ = old_; }
  };
  class AllowStronglyDiscouragedScope {
    Assembler* assembler_;
    bool old_;

   public:
    explicit AllowStronglyDiscouragedScope(Assembler* assembler)
        : assembler_(assembler), old_(assembler->allow_strongly_discouraged_) {
      assembler_->allow_strongly_discouraged_ = true;
    }
    ~AllowStronglyDiscouragedScope() {
      assembler_->allow_strongly_discouraged_ = old_;
    }
  };

  explicit Assembler(InstructionSet isa = kDefaultISA)
      : isa_(isa),
        first_condition_(al),
        it_mask_(0),
        has_32_dregs_(true),
        allow_unpredictable_(false),
        allow_strongly_discouraged_(false) {
#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
    // Avoid compiler warning.
    USE(isa_);
    VIXL_ASSERT(isa == A32);
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
    USE(isa_);
    VIXL_ASSERT(isa == T32);
#endif
  }
  explicit Assembler(size_t capacity, InstructionSet isa = kDefaultISA)
      : AssemblerBase(capacity),
        isa_(isa),
        first_condition_(al),
        it_mask_(0),
        has_32_dregs_(true),
        allow_unpredictable_(false),
        allow_strongly_discouraged_(false) {
#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
    VIXL_ASSERT(isa == A32);
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
    VIXL_ASSERT(isa == T32);
#endif
  }
  Assembler(byte* buffer, size_t capacity, InstructionSet isa = kDefaultISA)
      : AssemblerBase(buffer, capacity),
        isa_(isa),
        first_condition_(al),
        it_mask_(0),
        has_32_dregs_(true),
        allow_unpredictable_(false),
        allow_strongly_discouraged_(false) {
#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
    VIXL_ASSERT(isa == A32);
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
    VIXL_ASSERT(isa == T32);
#endif
  }
  virtual ~Assembler() {}

  void UseInstructionSet(InstructionSet isa) {
#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
    USE(isa);
    VIXL_ASSERT(isa == A32);
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
    USE(isa);
    VIXL_ASSERT(isa == T32);
#else
    VIXL_ASSERT((isa_ == isa) || (GetCursorOffset() == 0));
    isa_ = isa;
#endif
  }

#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
  InstructionSet GetInstructionSetInUse() const { return A32; }
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
  InstructionSet GetInstructionSetInUse() const { return T32; }
#else
  InstructionSet GetInstructionSetInUse() const { return isa_; }
#endif

  void UseT32() { UseInstructionSet(T32); }
  void UseA32() { UseInstructionSet(A32); }
  bool IsUsingT32() const { return GetInstructionSetInUse() == T32; }
  bool IsUsingA32() const { return GetInstructionSetInUse() == A32; }

  void SetIT(Condition first_condition, uint16_t it_mask) {
    VIXL_ASSERT(it_mask_ == 0);
    first_condition_ = first_condition;
    it_mask_ = it_mask;
  }
  bool InITBlock() { return it_mask_ != 0; }
  bool OutsideITBlock() { return it_mask_ == 0; }
  bool OutsideITBlockOrLast() { return (it_mask_ == 0) || (it_mask_ == 0x8); }
  bool OutsideITBlockAndAlOrLast(Condition cond) {
    return ((it_mask_ == 0) && cond.Is(al)) || (it_mask_ == 0x8);
  }
  void CheckNotIT() { VIXL_ASSERT(it_mask_ == 0); }
  bool Has32DRegs() const { return has_32_dregs_; }
  void SetHas32DRegs(bool has_32_dregs) { has_32_dregs_ = has_32_dregs; }

  int32_t GetCursorOffset() const {
    ptrdiff_t offset = buffer_.GetCursorOffset();
    VIXL_ASSERT(IsInt32(offset));
    return static_cast<int32_t>(offset);
  }

  uint32_t GetArchitectureStatePCOffset() const { return IsUsingT32() ? 4 : 8; }

  // Bind a raw Location that will never be tracked by the pool manager.
  void bind(Location* location) {
    VIXL_ASSERT(AllowAssembler());
    VIXL_ASSERT(!location->IsBound());
    location->SetLocation(this, GetCursorOffset());
    location->MarkBound();
  }

  // Bind a Label, which may be tracked by the pool manager in the presence of a
  // MacroAssembler.
  void bind(Label* label) {
    VIXL_ASSERT(AllowAssembler());
    BindHelper(label);
  }

  void place(RawLiteral* literal) {
    VIXL_ASSERT(AllowAssembler());
    VIXL_ASSERT(literal->IsManuallyPlaced());
    literal->SetLocation(this, GetCursorOffset());
    literal->MarkBound();
    GetBuffer()->EnsureSpaceFor(literal->GetSize());
    GetBuffer()->EmitData(literal->GetDataAddress(), literal->GetSize());
  }

  size_t GetSizeOfCodeGeneratedSince(Label* label) const {
    VIXL_ASSERT(label->IsBound());
    return buffer_.GetOffsetFrom(label->GetLocation());
  }

  // Helpers for it instruction.
  void it(Condition cond) { it(cond, 0x8); }
  void itt(Condition cond) { it(cond, 0x4); }
  void ite(Condition cond) { it(cond, 0xc); }
  void ittt(Condition cond) { it(cond, 0x2); }
  void itet(Condition cond) { it(cond, 0xa); }
  void itte(Condition cond) { it(cond, 0x6); }
  void itee(Condition cond) { it(cond, 0xe); }
  void itttt(Condition cond) { it(cond, 0x1); }
  void itett(Condition cond) { it(cond, 0x9); }
  void ittet(Condition cond) { it(cond, 0x5); }
  void iteet(Condition cond) { it(cond, 0xd); }
  void ittte(Condition cond) { it(cond, 0x3); }
  void itete(Condition cond) { it(cond, 0xb); }
  void ittee(Condition cond) { it(cond, 0x7); }
  void iteee(Condition cond) { it(cond, 0xf); }

  // Start of generated code.
  typedef void (Assembler::*InstructionCondSizeRROp)(Condition cond,
                                                     EncodingSize size,
                                                     Register rd,
                                                     Register rn,
                                                     const Operand& operand);
  typedef void (Assembler::*InstructionCondROp)(Condition cond,
                                                Register rd,
                                                const Operand& operand);
  typedef void (Assembler::*InstructionROp)(Register rd,
                                            const Operand& operand);
  typedef void (Assembler::*InstructionCondRROp)(Condition cond,
                                                 Register rd,
                                                 Register rn,
                                                 const Operand& operand);
  typedef void (Assembler::*InstructionCondSizeRL)(Condition cond,
                                                   EncodingSize size,
                                                   Register rd,
                                                   Location* location);
  typedef void (Assembler::*InstructionDtQQ)(DataType dt,
                                             QRegister rd,
                                             QRegister rm);
  typedef void (Assembler::*InstructionCondSizeL)(Condition cond,
                                                  EncodingSize size,
                                                  Location* location);
  typedef void (Assembler::*InstructionCondRII)(Condition cond,
                                                Register rd,
                                                uint32_t lsb,
                                                uint32_t width);
  typedef void (Assembler::*InstructionCondRRII)(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);
  typedef void (Assembler::*InstructionCondI)(Condition cond, uint32_t imm);
  typedef void (Assembler::*InstructionCondL)(Condition cond,
                                              Location* location);
  typedef void (Assembler::*InstructionCondR)(Condition cond, Register rm);
  typedef void (Assembler::*InstructionRL)(Register rn, Location* location);
  typedef void (Assembler::*InstructionCond)(Condition cond);
  typedef void (Assembler::*InstructionCondRR)(Condition cond,
                                               Register rd,
                                               Register rm);
  typedef void (Assembler::*InstructionCondSizeROp)(Condition cond,
                                                    EncodingSize size,
                                                    Register rn,
                                                    const Operand& operand);
  typedef void (Assembler::*InstructionCondRRR)(Condition cond,
                                                Register rd,
                                                Register rn,
                                                Register rm);
  typedef void (Assembler::*InstructionCondBa)(Condition cond,
                                               MemoryBarrier option);
  typedef void (Assembler::*InstructionCondRwbDrl)(Condition cond,
                                                   Register rn,
                                                   WriteBack write_back,
                                                   DRegisterList dreglist);
  typedef void (Assembler::*InstructionCondRMop)(Condition cond,
                                                 Register rt,
                                                 const MemOperand& operand);
  typedef void (Assembler::*InstructionCondRRMop)(Condition cond,
                                                  Register rt,
                                                  Register rt2,
                                                  const MemOperand& operand);
  typedef void (Assembler::*InstructionCondSizeRwbRl)(Condition cond,
                                                      EncodingSize size,
                                                      Register rn,
                                                      WriteBack write_back,
                                                      RegisterList registers);
  typedef void (Assembler::*InstructionCondRwbRl)(Condition cond,
                                                  Register rn,
                                                  WriteBack write_back,
                                                  RegisterList registers);
  typedef void (Assembler::*InstructionCondSizeRMop)(Condition cond,
                                                     EncodingSize size,
                                                     Register rt,
                                                     const MemOperand& operand);
  typedef void (Assembler::*InstructionCondRL)(Condition cond,
                                               Register rt,
                                               Location* location);
  typedef void (Assembler::*InstructionCondRRL)(Condition cond,
                                                Register rt,
                                                Register rt2,
                                                Location* location);
  typedef void (Assembler::*InstructionCondRRRR)(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  typedef void (Assembler::*InstructionCondRSr)(Condition cond,
                                                Register rd,
                                                SpecialRegister spec_reg);
  typedef void (Assembler::*InstructionCondMsrOp)(
      Condition cond, MaskedSpecialRegister spec_reg, const Operand& operand);
  typedef void (Assembler::*InstructionCondSizeRRR)(
      Condition cond, EncodingSize size, Register rd, Register rn, Register rm);
  typedef void (Assembler::*InstructionCondSize)(Condition cond,
                                                 EncodingSize size);
  typedef void (Assembler::*InstructionCondMop)(Condition cond,
                                                const MemOperand& operand);
  typedef void (Assembler::*InstructionCondSizeRl)(Condition cond,
                                                   EncodingSize size,
                                                   RegisterList registers);
  typedef void (Assembler::*InstructionCondSizeOrl)(Condition cond,
                                                    EncodingSize size,
                                                    Register rt);
  typedef void (Assembler::*InstructionCondSizeRR)(Condition cond,
                                                   EncodingSize size,
                                                   Register rd,
                                                   Register rm);
  typedef void (Assembler::*InstructionDtQQQ)(DataType dt,
                                              QRegister rd,
                                              QRegister rn,
                                              QRegister rm);
  typedef void (Assembler::*InstructionCondRIOp)(Condition cond,
                                                 Register rd,
                                                 uint32_t imm,
                                                 const Operand& operand);
  typedef void (Assembler::*InstructionCondRIR)(Condition cond,
                                                Register rd,
                                                uint32_t imm,
                                                Register rn);
  typedef void (Assembler::*InstructionCondRRRMop)(Condition cond,
                                                   Register rd,
                                                   Register rt,
                                                   Register rt2,
                                                   const MemOperand& operand);
  typedef void (Assembler::*InstructionCondSizeI)(Condition cond,
                                                  EncodingSize size,
                                                  uint32_t imm);
  typedef void (Assembler::*InstructionCondDtDDD)(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  typedef void (Assembler::*InstructionCondDtQQQ)(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  typedef void (Assembler::*InstructionCondDtQDD)(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  typedef void (Assembler::*InstructionCondDtDD)(Condition cond,
                                                 DataType dt,
                                                 DRegister rd,
                                                 DRegister rm);
  typedef void (Assembler::*InstructionCondDtQQ)(Condition cond,
                                                 DataType dt,
                                                 QRegister rd,
                                                 QRegister rm);
  typedef void (Assembler::*InstructionCondDtSS)(Condition cond,
                                                 DataType dt,
                                                 SRegister rd,
                                                 SRegister rm);
  typedef void (Assembler::*InstructionCondDtSSS)(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  typedef void (Assembler::*InstructionCondDtDQQ)(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);
  typedef void (Assembler::*InstructionCondDtQQD)(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm);
  typedef void (Assembler::*InstructionCondDtDDDop)(Condition cond,
                                                    DataType dt,
                                                    DRegister rd,
                                                    DRegister rn,
                                                    const DOperand& operand);
  typedef void (Assembler::*InstructionCondDtQQQop)(Condition cond,
                                                    DataType dt,
                                                    QRegister rd,
                                                    QRegister rn,
                                                    const QOperand& operand);
  typedef void (Assembler::*InstructionCondDtSSop)(Condition cond,
                                                   DataType dt,
                                                   SRegister rd,
                                                   const SOperand& operand);
  typedef void (Assembler::*InstructionCondDtDDop)(Condition cond,
                                                   DataType dt,
                                                   DRegister rd,
                                                   const DOperand& operand);
  typedef void (Assembler::*InstructionCondDtDtDS)(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);
  typedef void (Assembler::*InstructionCondDtDtSD)(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);
  typedef void (Assembler::*InstructionCondDtDtDDSi)(Condition cond,
                                                     DataType dt1,
                                                     DataType dt2,
                                                     DRegister rd,
                                                     DRegister rm,
                                                     int32_t fbits);
  typedef void (Assembler::*InstructionCondDtDtQQSi)(Condition cond,
                                                     DataType dt1,
                                                     DataType dt2,
                                                     QRegister rd,
                                                     QRegister rm,
                                                     int32_t fbits);
  typedef void (Assembler::*InstructionCondDtDtSSSi)(Condition cond,
                                                     DataType dt1,
                                                     DataType dt2,
                                                     SRegister rd,
                                                     SRegister rm,
                                                     int32_t fbits);
  typedef void (Assembler::*InstructionCondDtDtDD)(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm);
  typedef void (Assembler::*InstructionCondDtDtQQ)(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, QRegister rm);
  typedef void (Assembler::*InstructionCondDtDtDQ)(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, QRegister rm);
  typedef void (Assembler::*InstructionCondDtDtQD)(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, DRegister rm);
  typedef void (Assembler::*InstructionCondDtDtSS)(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);
  typedef void (Assembler::*InstructionDtDtDD)(DataType dt1,
                                               DataType dt2,
                                               DRegister rd,
                                               DRegister rm);
  typedef void (Assembler::*InstructionDtDtQQ)(DataType dt1,
                                               DataType dt2,
                                               QRegister rd,
                                               QRegister rm);
  typedef void (Assembler::*InstructionDtDtSS)(DataType dt1,
                                               DataType dt2,
                                               SRegister rd,
                                               SRegister rm);
  typedef void (Assembler::*InstructionDtDtSD)(DataType dt1,
                                               DataType dt2,
                                               SRegister rd,
                                               DRegister rm);
  typedef void (Assembler::*InstructionCondDtQR)(Condition cond,
                                                 DataType dt,
                                                 QRegister rd,
                                                 Register rt);
  typedef void (Assembler::*InstructionCondDtDR)(Condition cond,
                                                 DataType dt,
                                                 DRegister rd,
                                                 Register rt);
  typedef void (Assembler::*InstructionCondDtDDx)(Condition cond,
                                                  DataType dt,
                                                  DRegister rd,
                                                  DRegisterLane rm);
  typedef void (Assembler::*InstructionCondDtQDx)(Condition cond,
                                                  DataType dt,
                                                  QRegister rd,
                                                  DRegisterLane rm);
  typedef void (Assembler::*InstructionCondDtDDDDop)(Condition cond,
                                                     DataType dt,
                                                     DRegister rd,
                                                     DRegister rn,
                                                     DRegister rm,
                                                     const DOperand& operand);
  typedef void (Assembler::*InstructionCondDtQQQQop)(Condition cond,
                                                     DataType dt,
                                                     QRegister rd,
                                                     QRegister rn,
                                                     QRegister rm,
                                                     const QOperand& operand);
  typedef void (Assembler::*InstructionCondDtNrlAmop)(
      Condition cond,
      DataType dt,
      const NeonRegisterList& nreglist,
      const AlignedMemOperand& operand);
  typedef void (Assembler::*InstructionCondDtNrlMop)(
      Condition cond,
      DataType dt,
      const NeonRegisterList& nreglist,
      const MemOperand& operand);
  typedef void (Assembler::*InstructionCondDtRwbDrl)(Condition cond,
                                                     DataType dt,
                                                     Register rn,
                                                     WriteBack write_back,
                                                     DRegisterList dreglist);
  typedef void (Assembler::*InstructionCondDtRwbSrl)(Condition cond,
                                                     DataType dt,
                                                     Register rn,
                                                     WriteBack write_back,
                                                     SRegisterList sreglist);
  typedef void (Assembler::*InstructionCondDtDL)(Condition cond,
                                                 DataType dt,
                                                 DRegister rd,
                                                 Location* location);
  typedef void (Assembler::*InstructionCondDtDMop)(Condition cond,
                                                   DataType dt,
                                                   DRegister rd,
                                                   const MemOperand& operand);
  typedef void (Assembler::*InstructionCondDtSL)(Condition cond,
                                                 DataType dt,
                                                 SRegister rd,
                                                 Location* location);
  typedef void (Assembler::*InstructionCondDtSMop)(Condition cond,
                                                   DataType dt,
                                                   SRegister rd,
                                                   const MemOperand& operand);
  typedef void (Assembler::*InstructionDtDDD)(DataType dt,
                                              DRegister rd,
                                              DRegister rn,
                                              DRegister rm);
  typedef void (Assembler::*InstructionDtSSS)(DataType dt,
                                              SRegister rd,
                                              SRegister rn,
                                              SRegister rm);
  typedef void (Assembler::*InstructionCondDtDDDx)(Condition cond,
                                                   DataType dt,
                                                   DRegister rd,
                                                   DRegister rn,
                                                   DRegisterLane rm);
  typedef void (Assembler::*InstructionCondDtQQDx)(Condition cond,
                                                   DataType dt,
                                                   QRegister rd,
                                                   QRegister rn,
                                                   DRegisterLane rm);
  typedef void (Assembler::*InstructionCondDtQDDx)(Condition cond,
                                                   DataType dt,
                                                   QRegister rd,
                                                   DRegister rn,
                                                   DRegisterLane rm);
  typedef void (Assembler::*InstructionCondRS)(Condition cond,
                                               Register rt,
                                               SRegister rn);
  typedef void (Assembler::*InstructionCondSR)(Condition cond,
                                               SRegister rn,
                                               Register rt);
  typedef void (Assembler::*InstructionCondRRD)(Condition cond,
                                                Register rt,
                                                Register rt2,
                                                DRegister rm);
  typedef void (Assembler::*InstructionCondDRR)(Condition cond,
                                                DRegister rm,
                                                Register rt,
                                                Register rt2);
  typedef void (Assembler::*InstructionCondRRSS)(
      Condition cond, Register rt, Register rt2, SRegister rm, SRegister rm1);
  typedef void (Assembler::*InstructionCondSSRR)(
      Condition cond, SRegister rm, SRegister rm1, Register rt, Register rt2);
  typedef void (Assembler::*InstructionCondDtDxR)(Condition cond,
                                                  DataType dt,
                                                  DRegisterLane rd,
                                                  Register rt);
  typedef void (Assembler::*InstructionCondDtQQop)(Condition cond,
                                                   DataType dt,
                                                   QRegister rd,
                                                   const QOperand& operand);
  typedef void (Assembler::*InstructionCondDtRDx)(Condition cond,
                                                  DataType dt,
                                                  Register rt,
                                                  DRegisterLane rn);
  typedef void (Assembler::*InstructionCondDtQD)(Condition cond,
                                                 DataType dt,
                                                 QRegister rd,
                                                 DRegister rm);
  typedef void (Assembler::*InstructionCondDtDQ)(Condition cond,
                                                 DataType dt,
                                                 DRegister rd,
                                                 QRegister rm);
  typedef void (Assembler::*InstructionCondRoaSfp)(Condition cond,
                                                   RegisterOrAPSR_nzcv rt,
                                                   SpecialFPRegister spec_reg);
  typedef void (Assembler::*InstructionCondSfpR)(Condition cond,
                                                 SpecialFPRegister spec_reg,
                                                 Register rt);
  typedef void (Assembler::*InstructionCondDtDDIr)(Condition cond,
                                                   DataType dt,
                                                   DRegister rd,
                                                   DRegister rn,
                                                   DRegister dm,
                                                   unsigned index);
  typedef void (Assembler::*InstructionCondDtQQIr)(Condition cond,
                                                   DataType dt,
                                                   QRegister rd,
                                                   QRegister rn,
                                                   DRegister dm,
                                                   unsigned index);
  typedef void (Assembler::*InstructionCondDtQDIr)(Condition cond,
                                                   DataType dt,
                                                   QRegister rd,
                                                   DRegister rn,
                                                   DRegister dm,
                                                   unsigned index);
  typedef void (Assembler::*InstructionCondDtDrl)(Condition cond,
                                                  DataType dt,
                                                  DRegisterList dreglist);
  typedef void (Assembler::*InstructionCondDtSrl)(Condition cond,
                                                  DataType dt,
                                                  SRegisterList sreglist);
  typedef void (Assembler::*InstructionCondDtDQQop)(Condition cond,
                                                    DataType dt,
                                                    DRegister rd,
                                                    QRegister rm,
                                                    const QOperand& operand);
  typedef void (Assembler::*InstructionDtDD)(DataType dt,
                                             DRegister rd,
                                             DRegister rm);
  typedef void (Assembler::*InstructionDtSS)(DataType dt,
                                             SRegister rd,
                                             SRegister rm);
  typedef void (Assembler::*InstructionCondDtQDDop)(Condition cond,
                                                    DataType dt,
                                                    QRegister rd,
                                                    DRegister rm,
                                                    const DOperand& operand);
  typedef void (Assembler::*InstructionCondDtDNrlD)(
      Condition cond,
      DataType dt,
      DRegister rd,
      const NeonRegisterList& nreglist,
      DRegister rm);
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRROp /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rd*/,
                        Register /*rn*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kAdc) || (type == kAdcs) || (type == kAdd) ||
                (type == kAdds) || (type == kAnd) || (type == kAnds) ||
                (type == kAsr) || (type == kAsrs) || (type == kBic) ||
                (type == kBics) || (type == kEor) || (type == kEors) ||
                (type == kLsl) || (type == kLsls) || (type == kLsr) ||
                (type == kLsrs) || (type == kOrr) || (type == kOrrs) ||
                (type == kRor) || (type == kRors) || (type == kRsb) ||
                (type == kRsbs) || (type == kSbc) || (type == kSbcs) ||
                (type == kSub) || (type == kSubs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondROp /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kAdd) || (type == kMovt) || (type == kMovw) ||
                (type == kSub) || (type == kSxtb16) || (type == kTeq) ||
                (type == kUxtb16));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionROp /*instruction*/,
                        Register /*rd*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kAdds) || (type == kSubs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRROp /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        Register /*rn*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kAddw) || (type == kOrn) || (type == kOrns) ||
                (type == kPkhbt) || (type == kPkhtb) || (type == kRsc) ||
                (type == kRscs) || (type == kSubw) || (type == kSxtab) ||
                (type == kSxtab16) || (type == kSxtah) || (type == kUxtab) ||
                (type == kUxtab16) || (type == kUxtah));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRL /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rd*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kAdr) || (type == kLdr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtQQ /*instruction*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVrinta) || (type == kVrintm) || (type == kVrintn) ||
                (type == kVrintp) || (type == kVrintx) || (type == kVrintz));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeL /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kB));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRII /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        uint32_t /*lsb*/,
                        uint32_t /*width*/) {
    USE(type);
    VIXL_ASSERT((type == kBfc));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRII /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        Register /*rn*/,
                        uint32_t /*lsb*/,
                        uint32_t /*width*/) {
    USE(type);
    VIXL_ASSERT((type == kBfi) || (type == kSbfx) || (type == kUbfx));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondI /*instruction*/,
                        Condition /*cond*/,
                        uint32_t /*imm*/) {
    USE(type);
    VIXL_ASSERT((type == kBkpt) || (type == kHlt) || (type == kHvc) ||
                (type == kSvc));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondL /*instruction*/,
                        Condition /*cond*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kBl) || (type == kBlx) || (type == kPld) ||
                (type == kPli));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondR /*instruction*/,
                        Condition /*cond*/,
                        Register /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kBlx) || (type == kBx) || (type == kBxj));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionRL /*instruction*/,
                        Register /*rn*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kCbnz) || (type == kCbz));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCond /*instruction*/,
                        Condition /*cond*/) {
    USE(type);
    VIXL_ASSERT((type == kClrex));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRR /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        Register /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kClz) || (type == kRbit) || (type == kRrx) ||
                (type == kRrxs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeROp /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rn*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kCmn) || (type == kCmp) || (type == kMov) ||
                (type == kMovs) || (type == kMvn) || (type == kMvns) ||
                (type == kSxtb) || (type == kSxth) || (type == kTst) ||
                (type == kUxtb) || (type == kUxth));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRR /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        Register /*rn*/,
                        Register /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kCrc32b) || (type == kCrc32cb) || (type == kCrc32ch) ||
                (type == kCrc32cw) || (type == kCrc32h) || (type == kCrc32w) ||
                (type == kMuls) || (type == kQadd) || (type == kQadd16) ||
                (type == kQadd8) || (type == kQasx) || (type == kQdadd) ||
                (type == kQdsub) || (type == kQsax) || (type == kQsub) ||
                (type == kQsub16) || (type == kQsub8) || (type == kSadd16) ||
                (type == kSadd8) || (type == kSasx) || (type == kSdiv) ||
                (type == kSel) || (type == kShadd16) || (type == kShadd8) ||
                (type == kShasx) || (type == kShsax) || (type == kShsub16) ||
                (type == kShsub8) || (type == kSmmul) || (type == kSmmulr) ||
                (type == kSmuad) || (type == kSmuadx) || (type == kSmulbb) ||
                (type == kSmulbt) || (type == kSmultb) || (type == kSmultt) ||
                (type == kSmulwb) || (type == kSmulwt) || (type == kSmusd) ||
                (type == kSmusdx) || (type == kSsax) || (type == kSsub16) ||
                (type == kSsub8) || (type == kUadd16) || (type == kUadd8) ||
                (type == kUasx) || (type == kUdiv) || (type == kUhadd16) ||
                (type == kUhadd8) || (type == kUhasx) || (type == kUhsax) ||
                (type == kUhsub16) || (type == kUhsub8) || (type == kUqadd16) ||
                (type == kUqadd8) || (type == kUqasx) || (type == kUqsax) ||
                (type == kUqsub16) || (type == kUqsub8) || (type == kUsad8) ||
                (type == kUsax) || (type == kUsub16) || (type == kUsub8));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondBa /*instruction*/,
                        Condition /*cond*/,
                        MemoryBarrier /*option*/) {
    USE(type);
    VIXL_ASSERT((type == kDmb) || (type == kDsb) || (type == kIsb));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRwbDrl /*instruction*/,
                        Condition /*cond*/,
                        Register /*rn*/,
                        WriteBack /*write_back*/,
                        DRegisterList /*dreglist*/) {
    USE(type);
    VIXL_ASSERT((type == kFldmdbx) || (type == kFldmiax) ||
                (type == kFstmdbx) || (type == kFstmiax));
    UnimplementedDelegate(type);
  }
  virtual void DelegateIt(Condition /*cond*/, uint16_t /*mask*/) {
    UnimplementedDelegate(kIt);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRMop /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kLda) || (type == kLdab) || (type == kLdaex) ||
                (type == kLdaexb) || (type == kLdaexh) || (type == kLdah) ||
                (type == kLdrex) || (type == kLdrexb) || (type == kLdrexh) ||
                (type == kStl) || (type == kStlb) || (type == kStlh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRMop /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        Register /*rt2*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kLdaexd) || (type == kLdrd) || (type == kLdrexd) ||
                (type == kStlex) || (type == kStlexb) || (type == kStlexh) ||
                (type == kStrd) || (type == kStrex) || (type == kStrexb) ||
                (type == kStrexh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRwbRl /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rn*/,
                        WriteBack /*write_back*/,
                        RegisterList /*registers*/) {
    USE(type);
    VIXL_ASSERT((type == kLdm) || (type == kLdmfd) || (type == kStm) ||
                (type == kStmdb) || (type == kStmea));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRwbRl /*instruction*/,
                        Condition /*cond*/,
                        Register /*rn*/,
                        WriteBack /*write_back*/,
                        RegisterList /*registers*/) {
    USE(type);
    VIXL_ASSERT((type == kLdmda) || (type == kLdmdb) || (type == kLdmea) ||
                (type == kLdmed) || (type == kLdmfa) || (type == kLdmib) ||
                (type == kStmda) || (type == kStmed) || (type == kStmfa) ||
                (type == kStmfd) || (type == kStmib));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRMop /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rt*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kLdr) || (type == kLdrb) || (type == kLdrh) ||
                (type == kLdrsb) || (type == kLdrsh) || (type == kStr) ||
                (type == kStrb) || (type == kStrh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRL /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kLdrb) || (type == kLdrh) || (type == kLdrsb) ||
                (type == kLdrsh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRL /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        Register /*rt2*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kLdrd));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRRR /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        Register /*rn*/,
                        Register /*rm*/,
                        Register /*ra*/) {
    USE(type);
    VIXL_ASSERT((type == kMla) || (type == kMlas) || (type == kMls) ||
                (type == kSmlabb) || (type == kSmlabt) || (type == kSmlad) ||
                (type == kSmladx) || (type == kSmlal) || (type == kSmlalbb) ||
                (type == kSmlalbt) || (type == kSmlald) || (type == kSmlaldx) ||
                (type == kSmlals) || (type == kSmlaltb) || (type == kSmlaltt) ||
                (type == kSmlatb) || (type == kSmlatt) || (type == kSmlawb) ||
                (type == kSmlawt) || (type == kSmlsd) || (type == kSmlsdx) ||
                (type == kSmlsld) || (type == kSmlsldx) || (type == kSmmla) ||
                (type == kSmmlar) || (type == kSmmls) || (type == kSmmlsr) ||
                (type == kSmull) || (type == kSmulls) || (type == kUmaal) ||
                (type == kUmlal) || (type == kUmlals) || (type == kUmull) ||
                (type == kUmulls) || (type == kUsada8));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRSr /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        SpecialRegister /*spec_reg*/) {
    USE(type);
    VIXL_ASSERT((type == kMrs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondMsrOp /*instruction*/,
                        Condition /*cond*/,
                        MaskedSpecialRegister /*spec_reg*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kMsr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRRR /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rd*/,
                        Register /*rn*/,
                        Register /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kMul));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSize /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/) {
    USE(type);
    VIXL_ASSERT((type == kNop) || (type == kYield));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondMop /*instruction*/,
                        Condition /*cond*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kPld) || (type == kPldw) || (type == kPli));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRl /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        RegisterList /*registers*/) {
    USE(type);
    VIXL_ASSERT((type == kPop) || (type == kPush));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeOrl /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rt*/) {
    USE(type);
    VIXL_ASSERT((type == kPop) || (type == kPush));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRR /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        Register /*rd*/,
                        Register /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kRev) || (type == kRev16) || (type == kRevsh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtQQQ /*instruction*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmaxnm) || (type == kVminnm));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRIOp /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        uint32_t /*imm*/,
                        const Operand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kSsat) || (type == kUsat));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRIR /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        uint32_t /*imm*/,
                        Register /*rn*/) {
    USE(type);
    VIXL_ASSERT((type == kSsat16) || (type == kUsat16));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRRMop /*instruction*/,
                        Condition /*cond*/,
                        Register /*rd*/,
                        Register /*rt*/,
                        Register /*rt2*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kStlexd) || (type == kStrexd));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeI /*instruction*/,
                        Condition /*cond*/,
                        EncodingSize /*size*/,
                        uint32_t /*imm*/) {
    USE(type);
    VIXL_ASSERT((type == kUdf));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVaba) || (type == kVabd) || (type == kVacge) ||
                (type == kVacgt) || (type == kVacle) || (type == kVaclt) ||
                (type == kVadd) || (type == kVbif) || (type == kVbit) ||
                (type == kVbsl) || (type == kVceq) || (type == kVcge) ||
                (type == kVcgt) || (type == kVcle) || (type == kVclt) ||
                (type == kVdiv) || (type == kVeor) || (type == kVfma) ||
                (type == kVfms) || (type == kVfnma) || (type == kVfnms) ||
                (type == kVhadd) || (type == kVhsub) || (type == kVmax) ||
                (type == kVmin) || (type == kVmla) || (type == kVmls) ||
                (type == kVmul) || (type == kVnmla) || (type == kVnmls) ||
                (type == kVnmul) || (type == kVpadd) || (type == kVpmax) ||
                (type == kVpmin) || (type == kVqadd) || (type == kVqdmulh) ||
                (type == kVqrdmulh) || (type == kVqrshl) || (type == kVqsub) ||
                (type == kVrecps) || (type == kVrhadd) || (type == kVrshl) ||
                (type == kVrsqrts) || (type == kVsub) || (type == kVtst));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQQ /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVaba) || (type == kVabd) || (type == kVacge) ||
                (type == kVacgt) || (type == kVacle) || (type == kVaclt) ||
                (type == kVadd) || (type == kVbif) || (type == kVbit) ||
                (type == kVbsl) || (type == kVceq) || (type == kVcge) ||
                (type == kVcgt) || (type == kVcle) || (type == kVclt) ||
                (type == kVeor) || (type == kVfma) || (type == kVfms) ||
                (type == kVhadd) || (type == kVhsub) || (type == kVmax) ||
                (type == kVmin) || (type == kVmla) || (type == kVmls) ||
                (type == kVmul) || (type == kVqadd) || (type == kVqdmulh) ||
                (type == kVqrdmulh) || (type == kVqrshl) || (type == kVqsub) ||
                (type == kVrecps) || (type == kVrhadd) || (type == kVrshl) ||
                (type == kVrsqrts) || (type == kVsub) || (type == kVtst));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQDD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVabal) || (type == kVabdl) || (type == kVaddl) ||
                (type == kVmlal) || (type == kVmlsl) || (type == kVmull) ||
                (type == kVqdmlal) || (type == kVqdmlsl) ||
                (type == kVqdmull) || (type == kVsubl));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVabs) || (type == kVcls) || (type == kVclz) ||
                (type == kVcnt) || (type == kVneg) || (type == kVpadal) ||
                (type == kVpaddl) || (type == kVqabs) || (type == kVqneg) ||
                (type == kVrecpe) || (type == kVrev16) || (type == kVrev32) ||
                (type == kVrev64) || (type == kVrintr) || (type == kVrintx) ||
                (type == kVrintz) || (type == kVrsqrte) || (type == kVsqrt) ||
                (type == kVswp) || (type == kVtrn) || (type == kVuzp) ||
                (type == kVzip));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQ /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVabs) || (type == kVcls) || (type == kVclz) ||
                (type == kVcnt) || (type == kVneg) || (type == kVpadal) ||
                (type == kVpaddl) || (type == kVqabs) || (type == kVqneg) ||
                (type == kVrecpe) || (type == kVrev16) || (type == kVrev32) ||
                (type == kVrev64) || (type == kVrsqrte) || (type == kVswp) ||
                (type == kVtrn) || (type == kVuzp) || (type == kVzip));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSS /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVabs) || (type == kVneg) || (type == kVrintr) ||
                (type == kVrintx) || (type == kVrintz) || (type == kVsqrt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSSS /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        SRegister /*rn*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVadd) || (type == kVdiv) || (type == kVfma) ||
                (type == kVfms) || (type == kVfnma) || (type == kVfnms) ||
                (type == kVmla) || (type == kVmls) || (type == kVmul) ||
                (type == kVnmla) || (type == kVnmls) || (type == kVnmul) ||
                (type == kVsub));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDQQ /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        QRegister /*rn*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVaddhn) || (type == kVraddhn) || (type == kVrsubhn) ||
                (type == kVsubhn));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVaddw) || (type == kVsubw));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDDop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rn*/,
                        const DOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVand) || (type == kVbic) || (type == kVceq) ||
                (type == kVcge) || (type == kVcgt) || (type == kVcle) ||
                (type == kVclt) || (type == kVorn) || (type == kVorr) ||
                (type == kVqshl) || (type == kVqshlu) || (type == kVrshr) ||
                (type == kVrsra) || (type == kVshl) || (type == kVshr) ||
                (type == kVsli) || (type == kVsra) || (type == kVsri));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQQop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        const QOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVand) || (type == kVbic) || (type == kVceq) ||
                (type == kVcge) || (type == kVcgt) || (type == kVcle) ||
                (type == kVclt) || (type == kVorn) || (type == kVorr) ||
                (type == kVqshl) || (type == kVqshlu) || (type == kVrshr) ||
                (type == kVrsra) || (type == kVshl) || (type == kVshr) ||
                (type == kVsli) || (type == kVsra) || (type == kVsri));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSSop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        const SOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVcmp) || (type == kVcmpe) || (type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        const DOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVcmp) || (type == kVcmpe) || (type == kVmov) ||
                (type == kVmvn));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtDS /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        DRegister /*rd*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt) || (type == kVcvtb) || (type == kVcvtt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtSD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        SRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt) || (type == kVcvtb) || (type == kVcvtr) ||
                (type == kVcvtt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtDDSi /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        DRegister /*rd*/,
                        DRegister /*rm*/,
                        int32_t /*fbits*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtQQSi /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        QRegister /*rd*/,
                        QRegister /*rm*/,
                        int32_t /*fbits*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtSSSi /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        SRegister /*rd*/,
                        SRegister /*rm*/,
                        int32_t /*fbits*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtDD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        DRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtQQ /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        QRegister /*rd*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtDQ /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        DRegister /*rd*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtQD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        QRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDtSS /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        SRegister /*rd*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvt) || (type == kVcvtb) || (type == kVcvtr) ||
                (type == kVcvtt));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtDtDD /*instruction*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        DRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvta) || (type == kVcvtm) || (type == kVcvtn) ||
                (type == kVcvtp));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtDtQQ /*instruction*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        QRegister /*rd*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvta) || (type == kVcvtm) || (type == kVcvtn) ||
                (type == kVcvtp));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtDtSS /*instruction*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        SRegister /*rd*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvta) || (type == kVcvtm) || (type == kVcvtn) ||
                (type == kVcvtp));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtDtSD /*instruction*/,
                        DataType /*dt1*/,
                        DataType /*dt2*/,
                        SRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVcvta) || (type == kVcvtm) || (type == kVcvtn) ||
                (type == kVcvtp));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQR /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        Register /*rt*/) {
    USE(type);
    VIXL_ASSERT((type == kVdup));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDR /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        Register /*rt*/) {
    USE(type);
    VIXL_ASSERT((type == kVdup));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDx /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegisterLane /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVdup));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQDx /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        DRegisterLane /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVdup));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDDDop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegister /*rm*/,
                        const DOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVext));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQQQop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        QRegister /*rm*/,
                        const QOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVext));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtNrlAmop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        const NeonRegisterList& /*nreglist*/,
                        const AlignedMemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVld1) || (type == kVld2) || (type == kVld3) ||
                (type == kVld4) || (type == kVst1) || (type == kVst2) ||
                (type == kVst3) || (type == kVst4));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtNrlMop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        const NeonRegisterList& /*nreglist*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVld3) || (type == kVst3));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtRwbDrl /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        Register /*rn*/,
                        WriteBack /*write_back*/,
                        DRegisterList /*dreglist*/) {
    USE(type);
    VIXL_ASSERT((type == kVldm) || (type == kVldmdb) || (type == kVldmia) ||
                (type == kVstm) || (type == kVstmdb) || (type == kVstmia));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtRwbSrl /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        Register /*rn*/,
                        WriteBack /*write_back*/,
                        SRegisterList /*sreglist*/) {
    USE(type);
    VIXL_ASSERT((type == kVldm) || (type == kVldmdb) || (type == kVldmia) ||
                (type == kVstm) || (type == kVstmdb) || (type == kVstmia));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDL /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kVldr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDMop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVldr) || (type == kVstr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSL /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        Location* /*location*/) {
    USE(type);
    VIXL_ASSERT((type == kVldr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSMop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        const MemOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVldr) || (type == kVstr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtDDD /*instruction*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmaxnm) || (type == kVminnm) || (type == kVseleq) ||
                (type == kVselge) || (type == kVselgt) || (type == kVselvs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtSSS /*instruction*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        SRegister /*rn*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmaxnm) || (type == kVminnm) || (type == kVseleq) ||
                (type == kVselge) || (type == kVselgt) || (type == kVselvs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDDx /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegisterLane /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmla) || (type == kVmls) || (type == kVqdmulh) ||
                (type == kVqrdmulh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQDx /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        DRegisterLane /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmla) || (type == kVmls) || (type == kVqdmulh) ||
                (type == kVqrdmulh));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQDDx /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegisterLane /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmlal) || (type == kVmlsl) || (type == kVqdmull));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRS /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        SRegister /*rn*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSR /*instruction*/,
                        Condition /*cond*/,
                        SRegister /*rn*/,
                        Register /*rt*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRD /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        Register /*rt2*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDRR /*instruction*/,
                        Condition /*cond*/,
                        DRegister /*rm*/,
                        Register /*rt*/,
                        Register /*rt2*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRRSS /*instruction*/,
                        Condition /*cond*/,
                        Register /*rt*/,
                        Register /*rt2*/,
                        SRegister /*rm*/,
                        SRegister /*rm1*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSSRR /*instruction*/,
                        Condition /*cond*/,
                        SRegister /*rm*/,
                        SRegister /*rm1*/,
                        Register /*rt*/,
                        Register /*rt2*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDxR /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegisterLane /*rd*/,
                        Register /*rt*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        const QOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov) || (type == kVmvn));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtRDx /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        Register /*rt*/,
                        DRegisterLane /*rn*/) {
    USE(type);
    VIXL_ASSERT((type == kVmov));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmovl));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDQ /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        QRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVmovn) || (type == kVqmovn) || (type == kVqmovun));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondRoaSfp /*instruction*/,
                        Condition /*cond*/,
                        RegisterOrAPSR_nzcv /*rt*/,
                        SpecialFPRegister /*spec_reg*/) {
    USE(type);
    VIXL_ASSERT((type == kVmrs));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondSfpR /*instruction*/,
                        Condition /*cond*/,
                        SpecialFPRegister /*spec_reg*/,
                        Register /*rt*/) {
    USE(type);
    VIXL_ASSERT((type == kVmsr));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDIr /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegister /*dm*/,
                        unsigned /*index*/) {
    USE(type);
    VIXL_ASSERT((type == kVmul));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQIr /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        QRegister /*rn*/,
                        DRegister /*dm*/,
                        unsigned /*index*/) {
    USE(type);
    VIXL_ASSERT((type == kVmul));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQDIr /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        DRegister /*rn*/,
                        DRegister /*dm*/,
                        unsigned /*index*/) {
    USE(type);
    VIXL_ASSERT((type == kVmull) || (type == kVqdmlal) || (type == kVqdmlsl));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDrl /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegisterList /*dreglist*/) {
    USE(type);
    VIXL_ASSERT((type == kVpop) || (type == kVpush));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSrl /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        SRegisterList /*sreglist*/) {
    USE(type);
    VIXL_ASSERT((type == kVpop) || (type == kVpush));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDQQop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        QRegister /*rm*/,
                        const QOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVqrshrn) || (type == kVqrshrun) ||
                (type == kVqshrn) || (type == kVqshrun) || (type == kVrshrn) ||
                (type == kVshrn));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtDD /*instruction*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVrinta) || (type == kVrintm) || (type == kVrintn) ||
                (type == kVrintp));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionDtSS /*instruction*/,
                        DataType /*dt*/,
                        SRegister /*rd*/,
                        SRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVrinta) || (type == kVrintm) || (type == kVrintn) ||
                (type == kVrintp));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQDDop /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        QRegister /*rd*/,
                        DRegister /*rm*/,
                        const DOperand& /*operand*/) {
    USE(type);
    VIXL_ASSERT((type == kVshll));
    UnimplementedDelegate(type);
  }
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDNrlD /*instruction*/,
                        Condition /*cond*/,
                        DataType /*dt*/,
                        DRegister /*rd*/,
                        const NeonRegisterList& /*nreglist*/,
                        DRegister /*rm*/) {
    USE(type);
    VIXL_ASSERT((type == kVtbl) || (type == kVtbx));
    UnimplementedDelegate(type);
  }

  void adc(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void adc(Register rd, Register rn, const Operand& operand) {
    adc(al, Best, rd, rn, operand);
  }
  void adc(Condition cond, Register rd, Register rn, const Operand& operand) {
    adc(cond, Best, rd, rn, operand);
  }
  void adc(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    adc(al, size, rd, rn, operand);
  }

  void adcs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void adcs(Register rd, Register rn, const Operand& operand) {
    adcs(al, Best, rd, rn, operand);
  }
  void adcs(Condition cond, Register rd, Register rn, const Operand& operand) {
    adcs(cond, Best, rd, rn, operand);
  }
  void adcs(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    adcs(al, size, rd, rn, operand);
  }

  void add(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void add(Register rd, Register rn, const Operand& operand) {
    add(al, Best, rd, rn, operand);
  }
  void add(Condition cond, Register rd, Register rn, const Operand& operand) {
    add(cond, Best, rd, rn, operand);
  }
  void add(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    add(al, size, rd, rn, operand);
  }

  void add(Condition cond, Register rd, const Operand& operand);
  void add(Register rd, const Operand& operand) { add(al, rd, operand); }

  void adds(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void adds(Register rd, Register rn, const Operand& operand) {
    adds(al, Best, rd, rn, operand);
  }
  void adds(Condition cond, Register rd, Register rn, const Operand& operand) {
    adds(cond, Best, rd, rn, operand);
  }
  void adds(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    adds(al, size, rd, rn, operand);
  }

  void adds(Register rd, const Operand& operand);

  void addw(Condition cond, Register rd, Register rn, const Operand& operand);
  void addw(Register rd, Register rn, const Operand& operand) {
    addw(al, rd, rn, operand);
  }

  void adr(Condition cond, EncodingSize size, Register rd, Location* location);
  bool adr_info(Condition cond,
                EncodingSize size,
                Register rd,
                Location* location,
                const struct ReferenceInfo** info);
  void adr(Register rd, Location* location) { adr(al, Best, rd, location); }
  void adr(Condition cond, Register rd, Location* location) {
    adr(cond, Best, rd, location);
  }
  void adr(EncodingSize size, Register rd, Location* location) {
    adr(al, size, rd, location);
  }

  void and_(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void and_(Register rd, Register rn, const Operand& operand) {
    and_(al, Best, rd, rn, operand);
  }
  void and_(Condition cond, Register rd, Register rn, const Operand& operand) {
    and_(cond, Best, rd, rn, operand);
  }
  void and_(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    and_(al, size, rd, rn, operand);
  }

  void ands(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void ands(Register rd, Register rn, const Operand& operand) {
    ands(al, Best, rd, rn, operand);
  }
  void ands(Condition cond, Register rd, Register rn, const Operand& operand) {
    ands(cond, Best, rd, rn, operand);
  }
  void ands(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    ands(al, size, rd, rn, operand);
  }

  void asr(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);
  void asr(Register rd, Register rm, const Operand& operand) {
    asr(al, Best, rd, rm, operand);
  }
  void asr(Condition cond, Register rd, Register rm, const Operand& operand) {
    asr(cond, Best, rd, rm, operand);
  }
  void asr(EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand) {
    asr(al, size, rd, rm, operand);
  }

  void asrs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);
  void asrs(Register rd, Register rm, const Operand& operand) {
    asrs(al, Best, rd, rm, operand);
  }
  void asrs(Condition cond, Register rd, Register rm, const Operand& operand) {
    asrs(cond, Best, rd, rm, operand);
  }
  void asrs(EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand) {
    asrs(al, size, rd, rm, operand);
  }

  void b(Condition cond, EncodingSize size, Location* location);
  bool b_info(Condition cond,
              EncodingSize size,
              Location* location,
              const struct ReferenceInfo** info);
  void b(Location* location) { b(al, Best, location); }
  void b(Condition cond, Location* location) { b(cond, Best, location); }
  void b(EncodingSize size, Location* location) { b(al, size, location); }

  void bfc(Condition cond, Register rd, uint32_t lsb, uint32_t width);
  void bfc(Register rd, uint32_t lsb, uint32_t width) {
    bfc(al, rd, lsb, width);
  }

  void bfi(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);
  void bfi(Register rd, Register rn, uint32_t lsb, uint32_t width) {
    bfi(al, rd, rn, lsb, width);
  }

  void bic(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void bic(Register rd, Register rn, const Operand& operand) {
    bic(al, Best, rd, rn, operand);
  }
  void bic(Condition cond, Register rd, Register rn, const Operand& operand) {
    bic(cond, Best, rd, rn, operand);
  }
  void bic(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    bic(al, size, rd, rn, operand);
  }

  void bics(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void bics(Register rd, Register rn, const Operand& operand) {
    bics(al, Best, rd, rn, operand);
  }
  void bics(Condition cond, Register rd, Register rn, const Operand& operand) {
    bics(cond, Best, rd, rn, operand);
  }
  void bics(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    bics(al, size, rd, rn, operand);
  }

  void bkpt(Condition cond, uint32_t imm);
  void bkpt(uint32_t imm) { bkpt(al, imm); }

  void bl(Condition cond, Location* location);
  bool bl_info(Condition cond,
               Location* location,
               const struct ReferenceInfo** info);
  void bl(Location* location) { bl(al, location); }

  void blx(Condition cond, Location* location);
  bool blx_info(Condition cond,
                Location* location,
                const struct ReferenceInfo** info);
  void blx(Location* location) { blx(al, location); }

  void blx(Condition cond, Register rm);
  void blx(Register rm) { blx(al, rm); }

  void bx(Condition cond, Register rm);
  void bx(Register rm) { bx(al, rm); }

  void bxj(Condition cond, Register rm);
  void bxj(Register rm) { bxj(al, rm); }

  void cbnz(Register rn, Location* location);
  bool cbnz_info(Register rn,
                 Location* location,
                 const struct ReferenceInfo** info);

  void cbz(Register rn, Location* location);
  bool cbz_info(Register rn,
                Location* location,
                const struct ReferenceInfo** info);

  void clrex(Condition cond);
  void clrex() { clrex(al); }

  void clz(Condition cond, Register rd, Register rm);
  void clz(Register rd, Register rm) { clz(al, rd, rm); }

  void cmn(Condition cond,
           EncodingSize size,
           Register rn,
           const Operand& operand);
  void cmn(Register rn, const Operand& operand) { cmn(al, Best, rn, operand); }
  void cmn(Condition cond, Register rn, const Operand& operand) {
    cmn(cond, Best, rn, operand);
  }
  void cmn(EncodingSize size, Register rn, const Operand& operand) {
    cmn(al, size, rn, operand);
  }

  void cmp(Condition cond,
           EncodingSize size,
           Register rn,
           const Operand& operand);
  void cmp(Register rn, const Operand& operand) { cmp(al, Best, rn, operand); }
  void cmp(Condition cond, Register rn, const Operand& operand) {
    cmp(cond, Best, rn, operand);
  }
  void cmp(EncodingSize size, Register rn, const Operand& operand) {
    cmp(al, size, rn, operand);
  }

  void crc32b(Condition cond, Register rd, Register rn, Register rm);
  void crc32b(Register rd, Register rn, Register rm) { crc32b(al, rd, rn, rm); }

  void crc32cb(Condition cond, Register rd, Register rn, Register rm);
  void crc32cb(Register rd, Register rn, Register rm) {
    crc32cb(al, rd, rn, rm);
  }

  void crc32ch(Condition cond, Register rd, Register rn, Register rm);
  void crc32ch(Register rd, Register rn, Register rm) {
    crc32ch(al, rd, rn, rm);
  }

  void crc32cw(Condition cond, Register rd, Register rn, Register rm);
  void crc32cw(Register rd, Register rn, Register rm) {
    crc32cw(al, rd, rn, rm);
  }

  void crc32h(Condition cond, Register rd, Register rn, Register rm);
  void crc32h(Register rd, Register rn, Register rm) { crc32h(al, rd, rn, rm); }

  void crc32w(Condition cond, Register rd, Register rn, Register rm);
  void crc32w(Register rd, Register rn, Register rm) { crc32w(al, rd, rn, rm); }

  void dmb(Condition cond, MemoryBarrier option);
  void dmb(MemoryBarrier option) { dmb(al, option); }

  void dsb(Condition cond, MemoryBarrier option);
  void dsb(MemoryBarrier option) { dsb(al, option); }

  void eor(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void eor(Register rd, Register rn, const Operand& operand) {
    eor(al, Best, rd, rn, operand);
  }
  void eor(Condition cond, Register rd, Register rn, const Operand& operand) {
    eor(cond, Best, rd, rn, operand);
  }
  void eor(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    eor(al, size, rd, rn, operand);
  }

  void eors(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void eors(Register rd, Register rn, const Operand& operand) {
    eors(al, Best, rd, rn, operand);
  }
  void eors(Condition cond, Register rd, Register rn, const Operand& operand) {
    eors(cond, Best, rd, rn, operand);
  }
  void eors(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    eors(al, size, rd, rn, operand);
  }

  void fldmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);
  void fldmdbx(Register rn, WriteBack write_back, DRegisterList dreglist) {
    fldmdbx(al, rn, write_back, dreglist);
  }

  void fldmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);
  void fldmiax(Register rn, WriteBack write_back, DRegisterList dreglist) {
    fldmiax(al, rn, write_back, dreglist);
  }

  void fstmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);
  void fstmdbx(Register rn, WriteBack write_back, DRegisterList dreglist) {
    fstmdbx(al, rn, write_back, dreglist);
  }

  void fstmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);
  void fstmiax(Register rn, WriteBack write_back, DRegisterList dreglist) {
    fstmiax(al, rn, write_back, dreglist);
  }

  void hlt(Condition cond, uint32_t imm);
  void hlt(uint32_t imm) { hlt(al, imm); }

  void hvc(Condition cond, uint32_t imm);
  void hvc(uint32_t imm) { hvc(al, imm); }

  void isb(Condition cond, MemoryBarrier option);
  void isb(MemoryBarrier option) { isb(al, option); }

  void it(Condition cond, uint16_t mask);

  void lda(Condition cond, Register rt, const MemOperand& operand);
  void lda(Register rt, const MemOperand& operand) { lda(al, rt, operand); }

  void ldab(Condition cond, Register rt, const MemOperand& operand);
  void ldab(Register rt, const MemOperand& operand) { ldab(al, rt, operand); }

  void ldaex(Condition cond, Register rt, const MemOperand& operand);
  void ldaex(Register rt, const MemOperand& operand) { ldaex(al, rt, operand); }

  void ldaexb(Condition cond, Register rt, const MemOperand& operand);
  void ldaexb(Register rt, const MemOperand& operand) {
    ldaexb(al, rt, operand);
  }

  void ldaexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand);
  void ldaexd(Register rt, Register rt2, const MemOperand& operand) {
    ldaexd(al, rt, rt2, operand);
  }

  void ldaexh(Condition cond, Register rt, const MemOperand& operand);
  void ldaexh(Register rt, const MemOperand& operand) {
    ldaexh(al, rt, operand);
  }

  void ldah(Condition cond, Register rt, const MemOperand& operand);
  void ldah(Register rt, const MemOperand& operand) { ldah(al, rt, operand); }

  void ldm(Condition cond,
           EncodingSize size,
           Register rn,
           WriteBack write_back,
           RegisterList registers);
  void ldm(Register rn, WriteBack write_back, RegisterList registers) {
    ldm(al, Best, rn, write_back, registers);
  }
  void ldm(Condition cond,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    ldm(cond, Best, rn, write_back, registers);
  }
  void ldm(EncodingSize size,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    ldm(al, size, rn, write_back, registers);
  }

  void ldmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmda(Register rn, WriteBack write_back, RegisterList registers) {
    ldmda(al, rn, write_back, registers);
  }

  void ldmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmdb(Register rn, WriteBack write_back, RegisterList registers) {
    ldmdb(al, rn, write_back, registers);
  }

  void ldmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmea(Register rn, WriteBack write_back, RegisterList registers) {
    ldmea(al, rn, write_back, registers);
  }

  void ldmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmed(Register rn, WriteBack write_back, RegisterList registers) {
    ldmed(al, rn, write_back, registers);
  }

  void ldmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmfa(Register rn, WriteBack write_back, RegisterList registers) {
    ldmfa(al, rn, write_back, registers);
  }

  void ldmfd(Condition cond,
             EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmfd(Register rn, WriteBack write_back, RegisterList registers) {
    ldmfd(al, Best, rn, write_back, registers);
  }
  void ldmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    ldmfd(cond, Best, rn, write_back, registers);
  }
  void ldmfd(EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    ldmfd(al, size, rn, write_back, registers);
  }

  void ldmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void ldmib(Register rn, WriteBack write_back, RegisterList registers) {
    ldmib(al, rn, write_back, registers);
  }

  void ldr(Condition cond,
           EncodingSize size,
           Register rt,
           const MemOperand& operand);
  void ldr(Register rt, const MemOperand& operand) {
    ldr(al, Best, rt, operand);
  }
  void ldr(Condition cond, Register rt, const MemOperand& operand) {
    ldr(cond, Best, rt, operand);
  }
  void ldr(EncodingSize size, Register rt, const MemOperand& operand) {
    ldr(al, size, rt, operand);
  }

  void ldr(Condition cond, EncodingSize size, Register rt, Location* location);
  bool ldr_info(Condition cond,
                EncodingSize size,
                Register rt,
                Location* location,
                const struct ReferenceInfo** info);
  void ldr(Register rt, Location* location) { ldr(al, Best, rt, location); }
  void ldr(Condition cond, Register rt, Location* location) {
    ldr(cond, Best, rt, location);
  }
  void ldr(EncodingSize size, Register rt, Location* location) {
    ldr(al, size, rt, location);
  }

  void ldrb(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);
  void ldrb(Register rt, const MemOperand& operand) {
    ldrb(al, Best, rt, operand);
  }
  void ldrb(Condition cond, Register rt, const MemOperand& operand) {
    ldrb(cond, Best, rt, operand);
  }
  void ldrb(EncodingSize size, Register rt, const MemOperand& operand) {
    ldrb(al, size, rt, operand);
  }

  void ldrb(Condition cond, Register rt, Location* location);
  bool ldrb_info(Condition cond,
                 Register rt,
                 Location* location,
                 const struct ReferenceInfo** info);
  void ldrb(Register rt, Location* location) { ldrb(al, rt, location); }

  void ldrd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand);
  void ldrd(Register rt, Register rt2, const MemOperand& operand) {
    ldrd(al, rt, rt2, operand);
  }

  void ldrd(Condition cond, Register rt, Register rt2, Location* location);
  bool ldrd_info(Condition cond,
                 Register rt,
                 Register rt2,
                 Location* location,
                 const struct ReferenceInfo** info);
  void ldrd(Register rt, Register rt2, Location* location) {
    ldrd(al, rt, rt2, location);
  }

  void ldrex(Condition cond, Register rt, const MemOperand& operand);
  void ldrex(Register rt, const MemOperand& operand) { ldrex(al, rt, operand); }

  void ldrexb(Condition cond, Register rt, const MemOperand& operand);
  void ldrexb(Register rt, const MemOperand& operand) {
    ldrexb(al, rt, operand);
  }

  void ldrexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand);
  void ldrexd(Register rt, Register rt2, const MemOperand& operand) {
    ldrexd(al, rt, rt2, operand);
  }

  void ldrexh(Condition cond, Register rt, const MemOperand& operand);
  void ldrexh(Register rt, const MemOperand& operand) {
    ldrexh(al, rt, operand);
  }

  void ldrh(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);
  void ldrh(Register rt, const MemOperand& operand) {
    ldrh(al, Best, rt, operand);
  }
  void ldrh(Condition cond, Register rt, const MemOperand& operand) {
    ldrh(cond, Best, rt, operand);
  }
  void ldrh(EncodingSize size, Register rt, const MemOperand& operand) {
    ldrh(al, size, rt, operand);
  }

  void ldrh(Condition cond, Register rt, Location* location);
  bool ldrh_info(Condition cond,
                 Register rt,
                 Location* location,
                 const struct ReferenceInfo** info);
  void ldrh(Register rt, Location* location) { ldrh(al, rt, location); }

  void ldrsb(Condition cond,
             EncodingSize size,
             Register rt,
             const MemOperand& operand);
  void ldrsb(Register rt, const MemOperand& operand) {
    ldrsb(al, Best, rt, operand);
  }
  void ldrsb(Condition cond, Register rt, const MemOperand& operand) {
    ldrsb(cond, Best, rt, operand);
  }
  void ldrsb(EncodingSize size, Register rt, const MemOperand& operand) {
    ldrsb(al, size, rt, operand);
  }

  void ldrsb(Condition cond, Register rt, Location* location);
  bool ldrsb_info(Condition cond,
                  Register rt,
                  Location* location,
                  const struct ReferenceInfo** info);
  void ldrsb(Register rt, Location* location) { ldrsb(al, rt, location); }

  void ldrsh(Condition cond,
             EncodingSize size,
             Register rt,
             const MemOperand& operand);
  void ldrsh(Register rt, const MemOperand& operand) {
    ldrsh(al, Best, rt, operand);
  }
  void ldrsh(Condition cond, Register rt, const MemOperand& operand) {
    ldrsh(cond, Best, rt, operand);
  }
  void ldrsh(EncodingSize size, Register rt, const MemOperand& operand) {
    ldrsh(al, size, rt, operand);
  }

  void ldrsh(Condition cond, Register rt, Location* location);
  bool ldrsh_info(Condition cond,
                  Register rt,
                  Location* location,
                  const struct ReferenceInfo** info);
  void ldrsh(Register rt, Location* location) { ldrsh(al, rt, location); }

  void lsl(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);
  void lsl(Register rd, Register rm, const Operand& operand) {
    lsl(al, Best, rd, rm, operand);
  }
  void lsl(Condition cond, Register rd, Register rm, const Operand& operand) {
    lsl(cond, Best, rd, rm, operand);
  }
  void lsl(EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand) {
    lsl(al, size, rd, rm, operand);
  }

  void lsls(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);
  void lsls(Register rd, Register rm, const Operand& operand) {
    lsls(al, Best, rd, rm, operand);
  }
  void lsls(Condition cond, Register rd, Register rm, const Operand& operand) {
    lsls(cond, Best, rd, rm, operand);
  }
  void lsls(EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand) {
    lsls(al, size, rd, rm, operand);
  }

  void lsr(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);
  void lsr(Register rd, Register rm, const Operand& operand) {
    lsr(al, Best, rd, rm, operand);
  }
  void lsr(Condition cond, Register rd, Register rm, const Operand& operand) {
    lsr(cond, Best, rd, rm, operand);
  }
  void lsr(EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand) {
    lsr(al, size, rd, rm, operand);
  }

  void lsrs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);
  void lsrs(Register rd, Register rm, const Operand& operand) {
    lsrs(al, Best, rd, rm, operand);
  }
  void lsrs(Condition cond, Register rd, Register rm, const Operand& operand) {
    lsrs(cond, Best, rd, rm, operand);
  }
  void lsrs(EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand) {
    lsrs(al, size, rd, rm, operand);
  }

  void mla(Condition cond, Register rd, Register rn, Register rm, Register ra);
  void mla(Register rd, Register rn, Register rm, Register ra) {
    mla(al, rd, rn, rm, ra);
  }

  void mlas(Condition cond, Register rd, Register rn, Register rm, Register ra);
  void mlas(Register rd, Register rn, Register rm, Register ra) {
    mlas(al, rd, rn, rm, ra);
  }

  void mls(Condition cond, Register rd, Register rn, Register rm, Register ra);
  void mls(Register rd, Register rn, Register rm, Register ra) {
    mls(al, rd, rn, rm, ra);
  }

  void mov(Condition cond,
           EncodingSize size,
           Register rd,
           const Operand& operand);
  void mov(Register rd, const Operand& operand) { mov(al, Best, rd, operand); }
  void mov(Condition cond, Register rd, const Operand& operand) {
    mov(cond, Best, rd, operand);
  }
  void mov(EncodingSize size, Register rd, const Operand& operand) {
    mov(al, size, rd, operand);
  }

  void movs(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);
  void movs(Register rd, const Operand& operand) {
    movs(al, Best, rd, operand);
  }
  void movs(Condition cond, Register rd, const Operand& operand) {
    movs(cond, Best, rd, operand);
  }
  void movs(EncodingSize size, Register rd, const Operand& operand) {
    movs(al, size, rd, operand);
  }

  void movt(Condition cond, Register rd, const Operand& operand);
  void movt(Register rd, const Operand& operand) { movt(al, rd, operand); }

  void movw(Condition cond, Register rd, const Operand& operand);
  void movw(Register rd, const Operand& operand) { movw(al, rd, operand); }

  void mrs(Condition cond, Register rd, SpecialRegister spec_reg);
  void mrs(Register rd, SpecialRegister spec_reg) { mrs(al, rd, spec_reg); }

  void msr(Condition cond,
           MaskedSpecialRegister spec_reg,
           const Operand& operand);
  void msr(MaskedSpecialRegister spec_reg, const Operand& operand) {
    msr(al, spec_reg, operand);
  }

  void mul(
      Condition cond, EncodingSize size, Register rd, Register rn, Register rm);
  void mul(Register rd, Register rn, Register rm) { mul(al, Best, rd, rn, rm); }
  void mul(Condition cond, Register rd, Register rn, Register rm) {
    mul(cond, Best, rd, rn, rm);
  }
  void mul(EncodingSize size, Register rd, Register rn, Register rm) {
    mul(al, size, rd, rn, rm);
  }

  void muls(Condition cond, Register rd, Register rn, Register rm);
  void muls(Register rd, Register rn, Register rm) { muls(al, rd, rn, rm); }

  void mvn(Condition cond,
           EncodingSize size,
           Register rd,
           const Operand& operand);
  void mvn(Register rd, const Operand& operand) { mvn(al, Best, rd, operand); }
  void mvn(Condition cond, Register rd, const Operand& operand) {
    mvn(cond, Best, rd, operand);
  }
  void mvn(EncodingSize size, Register rd, const Operand& operand) {
    mvn(al, size, rd, operand);
  }

  void mvns(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);
  void mvns(Register rd, const Operand& operand) {
    mvns(al, Best, rd, operand);
  }
  void mvns(Condition cond, Register rd, const Operand& operand) {
    mvns(cond, Best, rd, operand);
  }
  void mvns(EncodingSize size, Register rd, const Operand& operand) {
    mvns(al, size, rd, operand);
  }

  void nop(Condition cond, EncodingSize size);
  void nop() { nop(al, Best); }
  void nop(Condition cond) { nop(cond, Best); }
  void nop(EncodingSize size) { nop(al, size); }

  void orn(Condition cond, Register rd, Register rn, const Operand& operand);
  void orn(Register rd, Register rn, const Operand& operand) {
    orn(al, rd, rn, operand);
  }

  void orns(Condition cond, Register rd, Register rn, const Operand& operand);
  void orns(Register rd, Register rn, const Operand& operand) {
    orns(al, rd, rn, operand);
  }

  void orr(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void orr(Register rd, Register rn, const Operand& operand) {
    orr(al, Best, rd, rn, operand);
  }
  void orr(Condition cond, Register rd, Register rn, const Operand& operand) {
    orr(cond, Best, rd, rn, operand);
  }
  void orr(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    orr(al, size, rd, rn, operand);
  }

  void orrs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void orrs(Register rd, Register rn, const Operand& operand) {
    orrs(al, Best, rd, rn, operand);
  }
  void orrs(Condition cond, Register rd, Register rn, const Operand& operand) {
    orrs(cond, Best, rd, rn, operand);
  }
  void orrs(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    orrs(al, size, rd, rn, operand);
  }

  void pkhbt(Condition cond, Register rd, Register rn, const Operand& operand);
  void pkhbt(Register rd, Register rn, const Operand& operand) {
    pkhbt(al, rd, rn, operand);
  }

  void pkhtb(Condition cond, Register rd, Register rn, const Operand& operand);
  void pkhtb(Register rd, Register rn, const Operand& operand) {
    pkhtb(al, rd, rn, operand);
  }

  void pld(Condition cond, Location* location);
  bool pld_info(Condition cond,
                Location* location,
                const struct ReferenceInfo** info);
  void pld(Location* location) { pld(al, location); }

  void pld(Condition cond, const MemOperand& operand);
  void pld(const MemOperand& operand) { pld(al, operand); }

  void pldw(Condition cond, const MemOperand& operand);
  void pldw(const MemOperand& operand) { pldw(al, operand); }

  void pli(Condition cond, const MemOperand& operand);
  void pli(const MemOperand& operand) { pli(al, operand); }

  void pli(Condition cond, Location* location);
  bool pli_info(Condition cond,
                Location* location,
                const struct ReferenceInfo** info);
  void pli(Location* location) { pli(al, location); }

  void pop(Condition cond, EncodingSize size, RegisterList registers);
  void pop(RegisterList registers) { pop(al, Best, registers); }
  void pop(Condition cond, RegisterList registers) {
    pop(cond, Best, registers);
  }
  void pop(EncodingSize size, RegisterList registers) {
    pop(al, size, registers);
  }

  void pop(Condition cond, EncodingSize size, Register rt);
  void pop(Register rt) { pop(al, Best, rt); }
  void pop(Condition cond, Register rt) { pop(cond, Best, rt); }
  void pop(EncodingSize size, Register rt) { pop(al, size, rt); }

  void push(Condition cond, EncodingSize size, RegisterList registers);
  void push(RegisterList registers) { push(al, Best, registers); }
  void push(Condition cond, RegisterList registers) {
    push(cond, Best, registers);
  }
  void push(EncodingSize size, RegisterList registers) {
    push(al, size, registers);
  }

  void push(Condition cond, EncodingSize size, Register rt);
  void push(Register rt) { push(al, Best, rt); }
  void push(Condition cond, Register rt) { push(cond, Best, rt); }
  void push(EncodingSize size, Register rt) { push(al, size, rt); }

  void qadd(Condition cond, Register rd, Register rm, Register rn);
  void qadd(Register rd, Register rm, Register rn) { qadd(al, rd, rm, rn); }

  void qadd16(Condition cond, Register rd, Register rn, Register rm);
  void qadd16(Register rd, Register rn, Register rm) { qadd16(al, rd, rn, rm); }

  void qadd8(Condition cond, Register rd, Register rn, Register rm);
  void qadd8(Register rd, Register rn, Register rm) { qadd8(al, rd, rn, rm); }

  void qasx(Condition cond, Register rd, Register rn, Register rm);
  void qasx(Register rd, Register rn, Register rm) { qasx(al, rd, rn, rm); }

  void qdadd(Condition cond, Register rd, Register rm, Register rn);
  void qdadd(Register rd, Register rm, Register rn) { qdadd(al, rd, rm, rn); }

  void qdsub(Condition cond, Register rd, Register rm, Register rn);
  void qdsub(Register rd, Register rm, Register rn) { qdsub(al, rd, rm, rn); }

  void qsax(Condition cond, Register rd, Register rn, Register rm);
  void qsax(Register rd, Register rn, Register rm) { qsax(al, rd, rn, rm); }

  void qsub(Condition cond, Register rd, Register rm, Register rn);
  void qsub(Register rd, Register rm, Register rn) { qsub(al, rd, rm, rn); }

  void qsub16(Condition cond, Register rd, Register rn, Register rm);
  void qsub16(Register rd, Register rn, Register rm) { qsub16(al, rd, rn, rm); }

  void qsub8(Condition cond, Register rd, Register rn, Register rm);
  void qsub8(Register rd, Register rn, Register rm) { qsub8(al, rd, rn, rm); }

  void rbit(Condition cond, Register rd, Register rm);
  void rbit(Register rd, Register rm) { rbit(al, rd, rm); }

  void rev(Condition cond, EncodingSize size, Register rd, Register rm);
  void rev(Register rd, Register rm) { rev(al, Best, rd, rm); }
  void rev(Condition cond, Register rd, Register rm) {
    rev(cond, Best, rd, rm);
  }
  void rev(EncodingSize size, Register rd, Register rm) {
    rev(al, size, rd, rm);
  }

  void rev16(Condition cond, EncodingSize size, Register rd, Register rm);
  void rev16(Register rd, Register rm) { rev16(al, Best, rd, rm); }
  void rev16(Condition cond, Register rd, Register rm) {
    rev16(cond, Best, rd, rm);
  }
  void rev16(EncodingSize size, Register rd, Register rm) {
    rev16(al, size, rd, rm);
  }

  void revsh(Condition cond, EncodingSize size, Register rd, Register rm);
  void revsh(Register rd, Register rm) { revsh(al, Best, rd, rm); }
  void revsh(Condition cond, Register rd, Register rm) {
    revsh(cond, Best, rd, rm);
  }
  void revsh(EncodingSize size, Register rd, Register rm) {
    revsh(al, size, rd, rm);
  }

  void ror(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);
  void ror(Register rd, Register rm, const Operand& operand) {
    ror(al, Best, rd, rm, operand);
  }
  void ror(Condition cond, Register rd, Register rm, const Operand& operand) {
    ror(cond, Best, rd, rm, operand);
  }
  void ror(EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand) {
    ror(al, size, rd, rm, operand);
  }

  void rors(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);
  void rors(Register rd, Register rm, const Operand& operand) {
    rors(al, Best, rd, rm, operand);
  }
  void rors(Condition cond, Register rd, Register rm, const Operand& operand) {
    rors(cond, Best, rd, rm, operand);
  }
  void rors(EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand) {
    rors(al, size, rd, rm, operand);
  }

  void rrx(Condition cond, Register rd, Register rm);
  void rrx(Register rd, Register rm) { rrx(al, rd, rm); }

  void rrxs(Condition cond, Register rd, Register rm);
  void rrxs(Register rd, Register rm) { rrxs(al, rd, rm); }

  void rsb(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void rsb(Register rd, Register rn, const Operand& operand) {
    rsb(al, Best, rd, rn, operand);
  }
  void rsb(Condition cond, Register rd, Register rn, const Operand& operand) {
    rsb(cond, Best, rd, rn, operand);
  }
  void rsb(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    rsb(al, size, rd, rn, operand);
  }

  void rsbs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void rsbs(Register rd, Register rn, const Operand& operand) {
    rsbs(al, Best, rd, rn, operand);
  }
  void rsbs(Condition cond, Register rd, Register rn, const Operand& operand) {
    rsbs(cond, Best, rd, rn, operand);
  }
  void rsbs(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    rsbs(al, size, rd, rn, operand);
  }

  void rsc(Condition cond, Register rd, Register rn, const Operand& operand);
  void rsc(Register rd, Register rn, const Operand& operand) {
    rsc(al, rd, rn, operand);
  }

  void rscs(Condition cond, Register rd, Register rn, const Operand& operand);
  void rscs(Register rd, Register rn, const Operand& operand) {
    rscs(al, rd, rn, operand);
  }

  void sadd16(Condition cond, Register rd, Register rn, Register rm);
  void sadd16(Register rd, Register rn, Register rm) { sadd16(al, rd, rn, rm); }

  void sadd8(Condition cond, Register rd, Register rn, Register rm);
  void sadd8(Register rd, Register rn, Register rm) { sadd8(al, rd, rn, rm); }

  void sasx(Condition cond, Register rd, Register rn, Register rm);
  void sasx(Register rd, Register rn, Register rm) { sasx(al, rd, rn, rm); }

  void sbc(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void sbc(Register rd, Register rn, const Operand& operand) {
    sbc(al, Best, rd, rn, operand);
  }
  void sbc(Condition cond, Register rd, Register rn, const Operand& operand) {
    sbc(cond, Best, rd, rn, operand);
  }
  void sbc(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    sbc(al, size, rd, rn, operand);
  }

  void sbcs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void sbcs(Register rd, Register rn, const Operand& operand) {
    sbcs(al, Best, rd, rn, operand);
  }
  void sbcs(Condition cond, Register rd, Register rn, const Operand& operand) {
    sbcs(cond, Best, rd, rn, operand);
  }
  void sbcs(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    sbcs(al, size, rd, rn, operand);
  }

  void sbfx(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);
  void sbfx(Register rd, Register rn, uint32_t lsb, uint32_t width) {
    sbfx(al, rd, rn, lsb, width);
  }

  void sdiv(Condition cond, Register rd, Register rn, Register rm);
  void sdiv(Register rd, Register rn, Register rm) { sdiv(al, rd, rn, rm); }

  void sel(Condition cond, Register rd, Register rn, Register rm);
  void sel(Register rd, Register rn, Register rm) { sel(al, rd, rn, rm); }

  void shadd16(Condition cond, Register rd, Register rn, Register rm);
  void shadd16(Register rd, Register rn, Register rm) {
    shadd16(al, rd, rn, rm);
  }

  void shadd8(Condition cond, Register rd, Register rn, Register rm);
  void shadd8(Register rd, Register rn, Register rm) { shadd8(al, rd, rn, rm); }

  void shasx(Condition cond, Register rd, Register rn, Register rm);
  void shasx(Register rd, Register rn, Register rm) { shasx(al, rd, rn, rm); }

  void shsax(Condition cond, Register rd, Register rn, Register rm);
  void shsax(Register rd, Register rn, Register rm) { shsax(al, rd, rn, rm); }

  void shsub16(Condition cond, Register rd, Register rn, Register rm);
  void shsub16(Register rd, Register rn, Register rm) {
    shsub16(al, rd, rn, rm);
  }

  void shsub8(Condition cond, Register rd, Register rn, Register rm);
  void shsub8(Register rd, Register rn, Register rm) { shsub8(al, rd, rn, rm); }

  void smlabb(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlabb(Register rd, Register rn, Register rm, Register ra) {
    smlabb(al, rd, rn, rm, ra);
  }

  void smlabt(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlabt(Register rd, Register rn, Register rm, Register ra) {
    smlabt(al, rd, rn, rm, ra);
  }

  void smlad(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlad(Register rd, Register rn, Register rm, Register ra) {
    smlad(al, rd, rn, rm, ra);
  }

  void smladx(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smladx(Register rd, Register rn, Register rm, Register ra) {
    smladx(al, rd, rn, rm, ra);
  }

  void smlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlal(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlal(al, rdlo, rdhi, rn, rm);
  }

  void smlalbb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlalbb(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlalbb(al, rdlo, rdhi, rn, rm);
  }

  void smlalbt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlalbt(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlalbt(al, rdlo, rdhi, rn, rm);
  }

  void smlald(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlald(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlald(al, rdlo, rdhi, rn, rm);
  }

  void smlaldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlaldx(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlaldx(al, rdlo, rdhi, rn, rm);
  }

  void smlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlals(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlals(al, rdlo, rdhi, rn, rm);
  }

  void smlaltb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlaltb(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlaltb(al, rdlo, rdhi, rn, rm);
  }

  void smlaltt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlaltt(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlaltt(al, rdlo, rdhi, rn, rm);
  }

  void smlatb(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlatb(Register rd, Register rn, Register rm, Register ra) {
    smlatb(al, rd, rn, rm, ra);
  }

  void smlatt(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlatt(Register rd, Register rn, Register rm, Register ra) {
    smlatt(al, rd, rn, rm, ra);
  }

  void smlawb(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlawb(Register rd, Register rn, Register rm, Register ra) {
    smlawb(al, rd, rn, rm, ra);
  }

  void smlawt(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlawt(Register rd, Register rn, Register rm, Register ra) {
    smlawt(al, rd, rn, rm, ra);
  }

  void smlsd(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlsd(Register rd, Register rn, Register rm, Register ra) {
    smlsd(al, rd, rn, rm, ra);
  }

  void smlsdx(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smlsdx(Register rd, Register rn, Register rm, Register ra) {
    smlsdx(al, rd, rn, rm, ra);
  }

  void smlsld(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlsld(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlsld(al, rdlo, rdhi, rn, rm);
  }

  void smlsldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smlsldx(Register rdlo, Register rdhi, Register rn, Register rm) {
    smlsldx(al, rdlo, rdhi, rn, rm);
  }

  void smmla(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smmla(Register rd, Register rn, Register rm, Register ra) {
    smmla(al, rd, rn, rm, ra);
  }

  void smmlar(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smmlar(Register rd, Register rn, Register rm, Register ra) {
    smmlar(al, rd, rn, rm, ra);
  }

  void smmls(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smmls(Register rd, Register rn, Register rm, Register ra) {
    smmls(al, rd, rn, rm, ra);
  }

  void smmlsr(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void smmlsr(Register rd, Register rn, Register rm, Register ra) {
    smmlsr(al, rd, rn, rm, ra);
  }

  void smmul(Condition cond, Register rd, Register rn, Register rm);
  void smmul(Register rd, Register rn, Register rm) { smmul(al, rd, rn, rm); }

  void smmulr(Condition cond, Register rd, Register rn, Register rm);
  void smmulr(Register rd, Register rn, Register rm) { smmulr(al, rd, rn, rm); }

  void smuad(Condition cond, Register rd, Register rn, Register rm);
  void smuad(Register rd, Register rn, Register rm) { smuad(al, rd, rn, rm); }

  void smuadx(Condition cond, Register rd, Register rn, Register rm);
  void smuadx(Register rd, Register rn, Register rm) { smuadx(al, rd, rn, rm); }

  void smulbb(Condition cond, Register rd, Register rn, Register rm);
  void smulbb(Register rd, Register rn, Register rm) { smulbb(al, rd, rn, rm); }

  void smulbt(Condition cond, Register rd, Register rn, Register rm);
  void smulbt(Register rd, Register rn, Register rm) { smulbt(al, rd, rn, rm); }

  void smull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smull(Register rdlo, Register rdhi, Register rn, Register rm) {
    smull(al, rdlo, rdhi, rn, rm);
  }

  void smulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void smulls(Register rdlo, Register rdhi, Register rn, Register rm) {
    smulls(al, rdlo, rdhi, rn, rm);
  }

  void smultb(Condition cond, Register rd, Register rn, Register rm);
  void smultb(Register rd, Register rn, Register rm) { smultb(al, rd, rn, rm); }

  void smultt(Condition cond, Register rd, Register rn, Register rm);
  void smultt(Register rd, Register rn, Register rm) { smultt(al, rd, rn, rm); }

  void smulwb(Condition cond, Register rd, Register rn, Register rm);
  void smulwb(Register rd, Register rn, Register rm) { smulwb(al, rd, rn, rm); }

  void smulwt(Condition cond, Register rd, Register rn, Register rm);
  void smulwt(Register rd, Register rn, Register rm) { smulwt(al, rd, rn, rm); }

  void smusd(Condition cond, Register rd, Register rn, Register rm);
  void smusd(Register rd, Register rn, Register rm) { smusd(al, rd, rn, rm); }

  void smusdx(Condition cond, Register rd, Register rn, Register rm);
  void smusdx(Register rd, Register rn, Register rm) { smusdx(al, rd, rn, rm); }

  void ssat(Condition cond, Register rd, uint32_t imm, const Operand& operand);
  void ssat(Register rd, uint32_t imm, const Operand& operand) {
    ssat(al, rd, imm, operand);
  }

  void ssat16(Condition cond, Register rd, uint32_t imm, Register rn);
  void ssat16(Register rd, uint32_t imm, Register rn) {
    ssat16(al, rd, imm, rn);
  }

  void ssax(Condition cond, Register rd, Register rn, Register rm);
  void ssax(Register rd, Register rn, Register rm) { ssax(al, rd, rn, rm); }

  void ssub16(Condition cond, Register rd, Register rn, Register rm);
  void ssub16(Register rd, Register rn, Register rm) { ssub16(al, rd, rn, rm); }

  void ssub8(Condition cond, Register rd, Register rn, Register rm);
  void ssub8(Register rd, Register rn, Register rm) { ssub8(al, rd, rn, rm); }

  void stl(Condition cond, Register rt, const MemOperand& operand);
  void stl(Register rt, const MemOperand& operand) { stl(al, rt, operand); }

  void stlb(Condition cond, Register rt, const MemOperand& operand);
  void stlb(Register rt, const MemOperand& operand) { stlb(al, rt, operand); }

  void stlex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand);
  void stlex(Register rd, Register rt, const MemOperand& operand) {
    stlex(al, rd, rt, operand);
  }

  void stlexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);
  void stlexb(Register rd, Register rt, const MemOperand& operand) {
    stlexb(al, rd, rt, operand);
  }

  void stlexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand);
  void stlexd(Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    stlexd(al, rd, rt, rt2, operand);
  }

  void stlexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);
  void stlexh(Register rd, Register rt, const MemOperand& operand) {
    stlexh(al, rd, rt, operand);
  }

  void stlh(Condition cond, Register rt, const MemOperand& operand);
  void stlh(Register rt, const MemOperand& operand) { stlh(al, rt, operand); }

  void stm(Condition cond,
           EncodingSize size,
           Register rn,
           WriteBack write_back,
           RegisterList registers);
  void stm(Register rn, WriteBack write_back, RegisterList registers) {
    stm(al, Best, rn, write_back, registers);
  }
  void stm(Condition cond,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    stm(cond, Best, rn, write_back, registers);
  }
  void stm(EncodingSize size,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    stm(al, size, rn, write_back, registers);
  }

  void stmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmda(Register rn, WriteBack write_back, RegisterList registers) {
    stmda(al, rn, write_back, registers);
  }

  void stmdb(Condition cond,
             EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmdb(Register rn, WriteBack write_back, RegisterList registers) {
    stmdb(al, Best, rn, write_back, registers);
  }
  void stmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    stmdb(cond, Best, rn, write_back, registers);
  }
  void stmdb(EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    stmdb(al, size, rn, write_back, registers);
  }

  void stmea(Condition cond,
             EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmea(Register rn, WriteBack write_back, RegisterList registers) {
    stmea(al, Best, rn, write_back, registers);
  }
  void stmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    stmea(cond, Best, rn, write_back, registers);
  }
  void stmea(EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    stmea(al, size, rn, write_back, registers);
  }

  void stmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmed(Register rn, WriteBack write_back, RegisterList registers) {
    stmed(al, rn, write_back, registers);
  }

  void stmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmfa(Register rn, WriteBack write_back, RegisterList registers) {
    stmfa(al, rn, write_back, registers);
  }

  void stmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmfd(Register rn, WriteBack write_back, RegisterList registers) {
    stmfd(al, rn, write_back, registers);
  }

  void stmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);
  void stmib(Register rn, WriteBack write_back, RegisterList registers) {
    stmib(al, rn, write_back, registers);
  }

  void str(Condition cond,
           EncodingSize size,
           Register rt,
           const MemOperand& operand);
  void str(Register rt, const MemOperand& operand) {
    str(al, Best, rt, operand);
  }
  void str(Condition cond, Register rt, const MemOperand& operand) {
    str(cond, Best, rt, operand);
  }
  void str(EncodingSize size, Register rt, const MemOperand& operand) {
    str(al, size, rt, operand);
  }

  void strb(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);
  void strb(Register rt, const MemOperand& operand) {
    strb(al, Best, rt, operand);
  }
  void strb(Condition cond, Register rt, const MemOperand& operand) {
    strb(cond, Best, rt, operand);
  }
  void strb(EncodingSize size, Register rt, const MemOperand& operand) {
    strb(al, size, rt, operand);
  }

  void strd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand);
  void strd(Register rt, Register rt2, const MemOperand& operand) {
    strd(al, rt, rt2, operand);
  }

  void strex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand);
  void strex(Register rd, Register rt, const MemOperand& operand) {
    strex(al, rd, rt, operand);
  }

  void strexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);
  void strexb(Register rd, Register rt, const MemOperand& operand) {
    strexb(al, rd, rt, operand);
  }

  void strexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand);
  void strexd(Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    strexd(al, rd, rt, rt2, operand);
  }

  void strexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);
  void strexh(Register rd, Register rt, const MemOperand& operand) {
    strexh(al, rd, rt, operand);
  }

  void strh(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);
  void strh(Register rt, const MemOperand& operand) {
    strh(al, Best, rt, operand);
  }
  void strh(Condition cond, Register rt, const MemOperand& operand) {
    strh(cond, Best, rt, operand);
  }
  void strh(EncodingSize size, Register rt, const MemOperand& operand) {
    strh(al, size, rt, operand);
  }

  void sub(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);
  void sub(Register rd, Register rn, const Operand& operand) {
    sub(al, Best, rd, rn, operand);
  }
  void sub(Condition cond, Register rd, Register rn, const Operand& operand) {
    sub(cond, Best, rd, rn, operand);
  }
  void sub(EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand) {
    sub(al, size, rd, rn, operand);
  }

  void sub(Condition cond, Register rd, const Operand& operand);
  void sub(Register rd, const Operand& operand) { sub(al, rd, operand); }

  void subs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);
  void subs(Register rd, Register rn, const Operand& operand) {
    subs(al, Best, rd, rn, operand);
  }
  void subs(Condition cond, Register rd, Register rn, const Operand& operand) {
    subs(cond, Best, rd, rn, operand);
  }
  void subs(EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand) {
    subs(al, size, rd, rn, operand);
  }

  void subs(Register rd, const Operand& operand);

  void subw(Condition cond, Register rd, Register rn, const Operand& operand);
  void subw(Register rd, Register rn, const Operand& operand) {
    subw(al, rd, rn, operand);
  }

  void svc(Condition cond, uint32_t imm);
  void svc(uint32_t imm) { svc(al, imm); }

  void sxtab(Condition cond, Register rd, Register rn, const Operand& operand);
  void sxtab(Register rd, Register rn, const Operand& operand) {
    sxtab(al, rd, rn, operand);
  }

  void sxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand);
  void sxtab16(Register rd, Register rn, const Operand& operand) {
    sxtab16(al, rd, rn, operand);
  }

  void sxtah(Condition cond, Register rd, Register rn, const Operand& operand);
  void sxtah(Register rd, Register rn, const Operand& operand) {
    sxtah(al, rd, rn, operand);
  }

  void sxtb(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);
  void sxtb(Register rd, const Operand& operand) {
    sxtb(al, Best, rd, operand);
  }
  void sxtb(Condition cond, Register rd, const Operand& operand) {
    sxtb(cond, Best, rd, operand);
  }
  void sxtb(EncodingSize size, Register rd, const Operand& operand) {
    sxtb(al, size, rd, operand);
  }

  void sxtb16(Condition cond, Register rd, const Operand& operand);
  void sxtb16(Register rd, const Operand& operand) { sxtb16(al, rd, operand); }

  void sxth(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);
  void sxth(Register rd, const Operand& operand) {
    sxth(al, Best, rd, operand);
  }
  void sxth(Condition cond, Register rd, const Operand& operand) {
    sxth(cond, Best, rd, operand);
  }
  void sxth(EncodingSize size, Register rd, const Operand& operand) {
    sxth(al, size, rd, operand);
  }

  void tbb(Condition cond, Register rn, Register rm);
  void tbb(Register rn, Register rm) { tbb(al, rn, rm); }

  void tbh(Condition cond, Register rn, Register rm);
  void tbh(Register rn, Register rm) { tbh(al, rn, rm); }

  void teq(Condition cond, Register rn, const Operand& operand);
  void teq(Register rn, const Operand& operand) { teq(al, rn, operand); }

  void tst(Condition cond,
           EncodingSize size,
           Register rn,
           const Operand& operand);
  void tst(Register rn, const Operand& operand) { tst(al, Best, rn, operand); }
  void tst(Condition cond, Register rn, const Operand& operand) {
    tst(cond, Best, rn, operand);
  }
  void tst(EncodingSize size, Register rn, const Operand& operand) {
    tst(al, size, rn, operand);
  }

  void uadd16(Condition cond, Register rd, Register rn, Register rm);
  void uadd16(Register rd, Register rn, Register rm) { uadd16(al, rd, rn, rm); }

  void uadd8(Condition cond, Register rd, Register rn, Register rm);
  void uadd8(Register rd, Register rn, Register rm) { uadd8(al, rd, rn, rm); }

  void uasx(Condition cond, Register rd, Register rn, Register rm);
  void uasx(Register rd, Register rn, Register rm) { uasx(al, rd, rn, rm); }

  void ubfx(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);
  void ubfx(Register rd, Register rn, uint32_t lsb, uint32_t width) {
    ubfx(al, rd, rn, lsb, width);
  }

  void udf(Condition cond, EncodingSize size, uint32_t imm);
  void udf(uint32_t imm) { udf(al, Best, imm); }
  void udf(Condition cond, uint32_t imm) { udf(cond, Best, imm); }
  void udf(EncodingSize size, uint32_t imm) { udf(al, size, imm); }

  void udiv(Condition cond, Register rd, Register rn, Register rm);
  void udiv(Register rd, Register rn, Register rm) { udiv(al, rd, rn, rm); }

  void uhadd16(Condition cond, Register rd, Register rn, Register rm);
  void uhadd16(Register rd, Register rn, Register rm) {
    uhadd16(al, rd, rn, rm);
  }

  void uhadd8(Condition cond, Register rd, Register rn, Register rm);
  void uhadd8(Register rd, Register rn, Register rm) { uhadd8(al, rd, rn, rm); }

  void uhasx(Condition cond, Register rd, Register rn, Register rm);
  void uhasx(Register rd, Register rn, Register rm) { uhasx(al, rd, rn, rm); }

  void uhsax(Condition cond, Register rd, Register rn, Register rm);
  void uhsax(Register rd, Register rn, Register rm) { uhsax(al, rd, rn, rm); }

  void uhsub16(Condition cond, Register rd, Register rn, Register rm);
  void uhsub16(Register rd, Register rn, Register rm) {
    uhsub16(al, rd, rn, rm);
  }

  void uhsub8(Condition cond, Register rd, Register rn, Register rm);
  void uhsub8(Register rd, Register rn, Register rm) { uhsub8(al, rd, rn, rm); }

  void umaal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void umaal(Register rdlo, Register rdhi, Register rn, Register rm) {
    umaal(al, rdlo, rdhi, rn, rm);
  }

  void umlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void umlal(Register rdlo, Register rdhi, Register rn, Register rm) {
    umlal(al, rdlo, rdhi, rn, rm);
  }

  void umlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void umlals(Register rdlo, Register rdhi, Register rn, Register rm) {
    umlals(al, rdlo, rdhi, rn, rm);
  }

  void umull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void umull(Register rdlo, Register rdhi, Register rn, Register rm) {
    umull(al, rdlo, rdhi, rn, rm);
  }

  void umulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);
  void umulls(Register rdlo, Register rdhi, Register rn, Register rm) {
    umulls(al, rdlo, rdhi, rn, rm);
  }

  void uqadd16(Condition cond, Register rd, Register rn, Register rm);
  void uqadd16(Register rd, Register rn, Register rm) {
    uqadd16(al, rd, rn, rm);
  }

  void uqadd8(Condition cond, Register rd, Register rn, Register rm);
  void uqadd8(Register rd, Register rn, Register rm) { uqadd8(al, rd, rn, rm); }

  void uqasx(Condition cond, Register rd, Register rn, Register rm);
  void uqasx(Register rd, Register rn, Register rm) { uqasx(al, rd, rn, rm); }

  void uqsax(Condition cond, Register rd, Register rn, Register rm);
  void uqsax(Register rd, Register rn, Register rm) { uqsax(al, rd, rn, rm); }

  void uqsub16(Condition cond, Register rd, Register rn, Register rm);
  void uqsub16(Register rd, Register rn, Register rm) {
    uqsub16(al, rd, rn, rm);
  }

  void uqsub8(Condition cond, Register rd, Register rn, Register rm);
  void uqsub8(Register rd, Register rn, Register rm) { uqsub8(al, rd, rn, rm); }

  void usad8(Condition cond, Register rd, Register rn, Register rm);
  void usad8(Register rd, Register rn, Register rm) { usad8(al, rd, rn, rm); }

  void usada8(
      Condition cond, Register rd, Register rn, Register rm, Register ra);
  void usada8(Register rd, Register rn, Register rm, Register ra) {
    usada8(al, rd, rn, rm, ra);
  }

  void usat(Condition cond, Register rd, uint32_t imm, const Operand& operand);
  void usat(Register rd, uint32_t imm, const Operand& operand) {
    usat(al, rd, imm, operand);
  }

  void usat16(Condition cond, Register rd, uint32_t imm, Register rn);
  void usat16(Register rd, uint32_t imm, Register rn) {
    usat16(al, rd, imm, rn);
  }

  void usax(Condition cond, Register rd, Register rn, Register rm);
  void usax(Register rd, Register rn, Register rm) { usax(al, rd, rn, rm); }

  void usub16(Condition cond, Register rd, Register rn, Register rm);
  void usub16(Register rd, Register rn, Register rm) { usub16(al, rd, rn, rm); }

  void usub8(Condition cond, Register rd, Register rn, Register rm);
  void usub8(Register rd, Register rn, Register rm) { usub8(al, rd, rn, rm); }

  void uxtab(Condition cond, Register rd, Register rn, const Operand& operand);
  void uxtab(Register rd, Register rn, const Operand& operand) {
    uxtab(al, rd, rn, operand);
  }

  void uxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand);
  void uxtab16(Register rd, Register rn, const Operand& operand) {
    uxtab16(al, rd, rn, operand);
  }

  void uxtah(Condition cond, Register rd, Register rn, const Operand& operand);
  void uxtah(Register rd, Register rn, const Operand& operand) {
    uxtah(al, rd, rn, operand);
  }

  void uxtb(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);
  void uxtb(Register rd, const Operand& operand) {
    uxtb(al, Best, rd, operand);
  }
  void uxtb(Condition cond, Register rd, const Operand& operand) {
    uxtb(cond, Best, rd, operand);
  }
  void uxtb(EncodingSize size, Register rd, const Operand& operand) {
    uxtb(al, size, rd, operand);
  }

  void uxtb16(Condition cond, Register rd, const Operand& operand);
  void uxtb16(Register rd, const Operand& operand) { uxtb16(al, rd, operand); }

  void uxth(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);
  void uxth(Register rd, const Operand& operand) {
    uxth(al, Best, rd, operand);
  }
  void uxth(Condition cond, Register rd, const Operand& operand) {
    uxth(cond, Best, rd, operand);
  }
  void uxth(EncodingSize size, Register rd, const Operand& operand) {
    uxth(al, size, rd, operand);
  }

  void vaba(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vaba(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vaba(al, dt, rd, rn, rm);
  }

  void vaba(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vaba(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vaba(al, dt, rd, rn, rm);
  }

  void vabal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vabal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vabal(al, dt, rd, rn, rm);
  }

  void vabd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vabd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vabd(al, dt, rd, rn, rm);
  }

  void vabd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vabd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vabd(al, dt, rd, rn, rm);
  }

  void vabdl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vabdl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vabdl(al, dt, rd, rn, rm);
  }

  void vabs(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vabs(DataType dt, DRegister rd, DRegister rm) { vabs(al, dt, rd, rm); }

  void vabs(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vabs(DataType dt, QRegister rd, QRegister rm) { vabs(al, dt, rd, rm); }

  void vabs(Condition cond, DataType dt, SRegister rd, SRegister rm);
  void vabs(DataType dt, SRegister rd, SRegister rm) { vabs(al, dt, rd, rm); }

  void vacge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vacge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vacge(al, dt, rd, rn, rm);
  }

  void vacge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vacge(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vacge(al, dt, rd, rn, rm);
  }

  void vacgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vacgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vacgt(al, dt, rd, rn, rm);
  }

  void vacgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vacgt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vacgt(al, dt, rd, rn, rm);
  }

  void vacle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vacle(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vacle(al, dt, rd, rn, rm);
  }

  void vacle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vacle(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vacle(al, dt, rd, rn, rm);
  }

  void vaclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vaclt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vaclt(al, dt, rd, rn, rm);
  }

  void vaclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vaclt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vaclt(al, dt, rd, rn, rm);
  }

  void vadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vadd(al, dt, rd, rn, rm);
  }

  void vadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vadd(al, dt, rd, rn, rm);
  }

  void vadd(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vadd(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vadd(al, dt, rd, rn, rm);
  }

  void vaddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);
  void vaddhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    vaddhn(al, dt, rd, rn, rm);
  }

  void vaddl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vaddl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vaddl(al, dt, rd, rn, rm);
  }

  void vaddw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm);
  void vaddw(DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    vaddw(al, dt, rd, rn, rm);
  }

  void vand(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);
  void vand(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    vand(al, dt, rd, rn, operand);
  }

  void vand(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);
  void vand(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    vand(al, dt, rd, rn, operand);
  }

  void vbic(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);
  void vbic(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    vbic(al, dt, rd, rn, operand);
  }

  void vbic(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);
  void vbic(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    vbic(al, dt, rd, rn, operand);
  }

  void vbif(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vbif(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vbif(al, dt, rd, rn, rm);
  }
  void vbif(DRegister rd, DRegister rn, DRegister rm) {
    vbif(al, kDataTypeValueNone, rd, rn, rm);
  }
  void vbif(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    vbif(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vbif(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vbif(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vbif(al, dt, rd, rn, rm);
  }
  void vbif(QRegister rd, QRegister rn, QRegister rm) {
    vbif(al, kDataTypeValueNone, rd, rn, rm);
  }
  void vbif(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    vbif(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vbit(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vbit(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vbit(al, dt, rd, rn, rm);
  }
  void vbit(DRegister rd, DRegister rn, DRegister rm) {
    vbit(al, kDataTypeValueNone, rd, rn, rm);
  }
  void vbit(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    vbit(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vbit(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vbit(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vbit(al, dt, rd, rn, rm);
  }
  void vbit(QRegister rd, QRegister rn, QRegister rm) {
    vbit(al, kDataTypeValueNone, rd, rn, rm);
  }
  void vbit(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    vbit(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vbsl(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vbsl(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vbsl(al, dt, rd, rn, rm);
  }
  void vbsl(DRegister rd, DRegister rn, DRegister rm) {
    vbsl(al, kDataTypeValueNone, rd, rn, rm);
  }
  void vbsl(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    vbsl(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vbsl(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vbsl(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vbsl(al, dt, rd, rn, rm);
  }
  void vbsl(QRegister rd, QRegister rn, QRegister rm) {
    vbsl(al, kDataTypeValueNone, rd, rn, rm);
  }
  void vbsl(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    vbsl(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vceq(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vceq(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vceq(al, dt, rd, rm, operand);
  }

  void vceq(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vceq(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vceq(al, dt, rd, rm, operand);
  }

  void vceq(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vceq(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vceq(al, dt, rd, rn, rm);
  }

  void vceq(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vceq(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vceq(al, dt, rd, rn, rm);
  }

  void vcge(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vcge(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vcge(al, dt, rd, rm, operand);
  }

  void vcge(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vcge(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vcge(al, dt, rd, rm, operand);
  }

  void vcge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vcge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vcge(al, dt, rd, rn, rm);
  }

  void vcge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vcge(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vcge(al, dt, rd, rn, rm);
  }

  void vcgt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vcgt(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vcgt(al, dt, rd, rm, operand);
  }

  void vcgt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vcgt(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vcgt(al, dt, rd, rm, operand);
  }

  void vcgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vcgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vcgt(al, dt, rd, rn, rm);
  }

  void vcgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vcgt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vcgt(al, dt, rd, rn, rm);
  }

  void vcle(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vcle(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vcle(al, dt, rd, rm, operand);
  }

  void vcle(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vcle(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vcle(al, dt, rd, rm, operand);
  }

  void vcle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vcle(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vcle(al, dt, rd, rn, rm);
  }

  void vcle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vcle(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vcle(al, dt, rd, rn, rm);
  }

  void vcls(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vcls(DataType dt, DRegister rd, DRegister rm) { vcls(al, dt, rd, rm); }

  void vcls(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vcls(DataType dt, QRegister rd, QRegister rm) { vcls(al, dt, rd, rm); }

  void vclt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vclt(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vclt(al, dt, rd, rm, operand);
  }

  void vclt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vclt(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vclt(al, dt, rd, rm, operand);
  }

  void vclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vclt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vclt(al, dt, rd, rn, rm);
  }

  void vclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vclt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vclt(al, dt, rd, rn, rm);
  }

  void vclz(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vclz(DataType dt, DRegister rd, DRegister rm) { vclz(al, dt, rd, rm); }

  void vclz(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vclz(DataType dt, QRegister rd, QRegister rm) { vclz(al, dt, rd, rm); }

  void vcmp(Condition cond, DataType dt, SRegister rd, const SOperand& operand);
  void vcmp(DataType dt, SRegister rd, const SOperand& operand) {
    vcmp(al, dt, rd, operand);
  }

  void vcmp(Condition cond, DataType dt, DRegister rd, const DOperand& operand);
  void vcmp(DataType dt, DRegister rd, const DOperand& operand) {
    vcmp(al, dt, rd, operand);
  }

  void vcmpe(Condition cond,
             DataType dt,
             SRegister rd,
             const SOperand& operand);
  void vcmpe(DataType dt, SRegister rd, const SOperand& operand) {
    vcmpe(al, dt, rd, operand);
  }

  void vcmpe(Condition cond,
             DataType dt,
             DRegister rd,
             const DOperand& operand);
  void vcmpe(DataType dt, DRegister rd, const DOperand& operand) {
    vcmpe(al, dt, rd, operand);
  }

  void vcnt(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vcnt(DataType dt, DRegister rd, DRegister rm) { vcnt(al, dt, rd, rm); }

  void vcnt(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vcnt(DataType dt, QRegister rd, QRegister rm) { vcnt(al, dt, rd, rm); }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);
  void vcvt(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);
  void vcvt(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            DRegister rd,
            DRegister rm,
            int32_t fbits);
  void vcvt(
      DataType dt1, DataType dt2, DRegister rd, DRegister rm, int32_t fbits) {
    vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            QRegister rd,
            QRegister rm,
            int32_t fbits);
  void vcvt(
      DataType dt1, DataType dt2, QRegister rd, QRegister rm, int32_t fbits) {
    vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            SRegister rd,
            SRegister rm,
            int32_t fbits);
  void vcvt(
      DataType dt1, DataType dt2, SRegister rd, SRegister rm, int32_t fbits) {
    vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm);
  void vcvt(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, QRegister rm);
  void vcvt(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, QRegister rm);
  void vcvt(DataType dt1, DataType dt2, DRegister rd, QRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, DRegister rm);
  void vcvt(DataType dt1, DataType dt2, QRegister rd, DRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);
  void vcvt(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    vcvt(al, dt1, dt2, rd, rm);
  }

  void vcvta(DataType dt1, DataType dt2, DRegister rd, DRegister rm);

  void vcvta(DataType dt1, DataType dt2, QRegister rd, QRegister rm);

  void vcvta(DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvta(DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);
  void vcvtb(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    vcvtb(al, dt1, dt2, rd, rm);
  }

  void vcvtb(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);
  void vcvtb(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    vcvtb(al, dt1, dt2, rd, rm);
  }

  void vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);
  void vcvtb(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    vcvtb(al, dt1, dt2, rd, rm);
  }

  void vcvtm(DataType dt1, DataType dt2, DRegister rd, DRegister rm);

  void vcvtm(DataType dt1, DataType dt2, QRegister rd, QRegister rm);

  void vcvtm(DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvtm(DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvtn(DataType dt1, DataType dt2, DRegister rd, DRegister rm);

  void vcvtn(DataType dt1, DataType dt2, QRegister rd, QRegister rm);

  void vcvtn(DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvtn(DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvtp(DataType dt1, DataType dt2, DRegister rd, DRegister rm);

  void vcvtp(DataType dt1, DataType dt2, QRegister rd, QRegister rm);

  void vcvtp(DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvtp(DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);
  void vcvtr(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    vcvtr(al, dt1, dt2, rd, rm);
  }

  void vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);
  void vcvtr(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    vcvtr(al, dt1, dt2, rd, rm);
  }

  void vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);
  void vcvtt(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    vcvtt(al, dt1, dt2, rd, rm);
  }

  void vcvtt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);
  void vcvtt(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    vcvtt(al, dt1, dt2, rd, rm);
  }

  void vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);
  void vcvtt(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    vcvtt(al, dt1, dt2, rd, rm);
  }

  void vdiv(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vdiv(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vdiv(al, dt, rd, rn, rm);
  }

  void vdiv(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vdiv(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vdiv(al, dt, rd, rn, rm);
  }

  void vdup(Condition cond, DataType dt, QRegister rd, Register rt);
  void vdup(DataType dt, QRegister rd, Register rt) { vdup(al, dt, rd, rt); }

  void vdup(Condition cond, DataType dt, DRegister rd, Register rt);
  void vdup(DataType dt, DRegister rd, Register rt) { vdup(al, dt, rd, rt); }

  void vdup(Condition cond, DataType dt, DRegister rd, DRegisterLane rm);
  void vdup(DataType dt, DRegister rd, DRegisterLane rm) {
    vdup(al, dt, rd, rm);
  }

  void vdup(Condition cond, DataType dt, QRegister rd, DRegisterLane rm);
  void vdup(DataType dt, QRegister rd, DRegisterLane rm) {
    vdup(al, dt, rd, rm);
  }

  void veor(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void veor(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    veor(al, dt, rd, rn, rm);
  }
  void veor(DRegister rd, DRegister rn, DRegister rm) {
    veor(al, kDataTypeValueNone, rd, rn, rm);
  }
  void veor(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    veor(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void veor(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void veor(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    veor(al, dt, rd, rn, rm);
  }
  void veor(QRegister rd, QRegister rn, QRegister rm) {
    veor(al, kDataTypeValueNone, rd, rn, rm);
  }
  void veor(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    veor(cond, kDataTypeValueNone, rd, rn, rm);
  }

  void vext(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand);
  void vext(DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand) {
    vext(al, dt, rd, rn, rm, operand);
  }

  void vext(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand);
  void vext(DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand) {
    vext(al, dt, rd, rn, rm, operand);
  }

  void vfma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vfma(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vfma(al, dt, rd, rn, rm);
  }

  void vfma(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vfma(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vfma(al, dt, rd, rn, rm);
  }

  void vfma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vfma(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vfma(al, dt, rd, rn, rm);
  }

  void vfms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vfms(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vfms(al, dt, rd, rn, rm);
  }

  void vfms(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vfms(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vfms(al, dt, rd, rn, rm);
  }

  void vfms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vfms(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vfms(al, dt, rd, rn, rm);
  }

  void vfnma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vfnma(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vfnma(al, dt, rd, rn, rm);
  }

  void vfnma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vfnma(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vfnma(al, dt, rd, rn, rm);
  }

  void vfnms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vfnms(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vfnms(al, dt, rd, rn, rm);
  }

  void vfnms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vfnms(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vfnms(al, dt, rd, rn, rm);
  }

  void vhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vhadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vhadd(al, dt, rd, rn, rm);
  }

  void vhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vhadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vhadd(al, dt, rd, rn, rm);
  }

  void vhsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vhsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vhsub(al, dt, rd, rn, rm);
  }

  void vhsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vhsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vhsub(al, dt, rd, rn, rm);
  }

  void vld1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vld1(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vld1(al, dt, nreglist, operand);
  }

  void vld2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vld2(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vld2(al, dt, nreglist, operand);
  }

  void vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vld3(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vld3(al, dt, nreglist, operand);
  }

  void vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand);
  void vld3(DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    vld3(al, dt, nreglist, operand);
  }

  void vld4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vld4(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vld4(al, dt, nreglist, operand);
  }

  void vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist);
  void vldm(DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    vldm(al, dt, rn, write_back, dreglist);
  }
  void vldm(Register rn, WriteBack write_back, DRegisterList dreglist) {
    vldm(al, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void vldm(Condition cond,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    vldm(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist);
  void vldm(DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    vldm(al, dt, rn, write_back, sreglist);
  }
  void vldm(Register rn, WriteBack write_back, SRegisterList sreglist) {
    vldm(al, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void vldm(Condition cond,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    vldm(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);
  void vldmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vldmdb(al, dt, rn, write_back, dreglist);
  }
  void vldmdb(Register rn, WriteBack write_back, DRegisterList dreglist) {
    vldmdb(al, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void vldmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vldmdb(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);
  void vldmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vldmdb(al, dt, rn, write_back, sreglist);
  }
  void vldmdb(Register rn, WriteBack write_back, SRegisterList sreglist) {
    vldmdb(al, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void vldmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vldmdb(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);
  void vldmia(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vldmia(al, dt, rn, write_back, dreglist);
  }
  void vldmia(Register rn, WriteBack write_back, DRegisterList dreglist) {
    vldmia(al, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void vldmia(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vldmia(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);
  void vldmia(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vldmia(al, dt, rn, write_back, sreglist);
  }
  void vldmia(Register rn, WriteBack write_back, SRegisterList sreglist) {
    vldmia(al, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void vldmia(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vldmia(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void vldr(Condition cond, DataType dt, DRegister rd, Location* location);
  bool vldr_info(Condition cond,
                 DataType dt,
                 DRegister rd,
                 Location* location,
                 const struct ReferenceInfo** info);
  void vldr(DataType dt, DRegister rd, Location* location) {
    vldr(al, dt, rd, location);
  }
  void vldr(DRegister rd, Location* location) {
    vldr(al, Untyped64, rd, location);
  }
  void vldr(Condition cond, DRegister rd, Location* location) {
    vldr(cond, Untyped64, rd, location);
  }

  void vldr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand);
  void vldr(DataType dt, DRegister rd, const MemOperand& operand) {
    vldr(al, dt, rd, operand);
  }
  void vldr(DRegister rd, const MemOperand& operand) {
    vldr(al, Untyped64, rd, operand);
  }
  void vldr(Condition cond, DRegister rd, const MemOperand& operand) {
    vldr(cond, Untyped64, rd, operand);
  }

  void vldr(Condition cond, DataType dt, SRegister rd, Location* location);
  bool vldr_info(Condition cond,
                 DataType dt,
                 SRegister rd,
                 Location* location,
                 const struct ReferenceInfo** info);
  void vldr(DataType dt, SRegister rd, Location* location) {
    vldr(al, dt, rd, location);
  }
  void vldr(SRegister rd, Location* location) {
    vldr(al, Untyped32, rd, location);
  }
  void vldr(Condition cond, SRegister rd, Location* location) {
    vldr(cond, Untyped32, rd, location);
  }

  void vldr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand);
  void vldr(DataType dt, SRegister rd, const MemOperand& operand) {
    vldr(al, dt, rd, operand);
  }
  void vldr(SRegister rd, const MemOperand& operand) {
    vldr(al, Untyped32, rd, operand);
  }
  void vldr(Condition cond, SRegister rd, const MemOperand& operand) {
    vldr(cond, Untyped32, rd, operand);
  }

  void vmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vmax(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vmax(al, dt, rd, rn, rm);
  }

  void vmax(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vmax(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vmax(al, dt, rd, rn, rm);
  }

  void vmaxnm(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmaxnm(DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vmaxnm(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vmin(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vmin(al, dt, rd, rn, rm);
  }

  void vmin(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vmin(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vmin(al, dt, rd, rn, rm);
  }

  void vminnm(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vminnm(DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vminnm(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmla(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm);
  void vmla(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    vmla(al, dt, rd, rn, rm);
  }

  void vmla(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm);
  void vmla(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    vmla(al, dt, rd, rn, rm);
  }

  void vmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vmla(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vmla(al, dt, rd, rn, rm);
  }

  void vmla(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vmla(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vmla(al, dt, rd, rn, rm);
  }

  void vmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vmla(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vmla(al, dt, rd, rn, rm);
  }

  void vmlal(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm);
  void vmlal(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    vmlal(al, dt, rd, rn, rm);
  }

  void vmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vmlal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vmlal(al, dt, rd, rn, rm);
  }

  void vmls(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm);
  void vmls(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    vmls(al, dt, rd, rn, rm);
  }

  void vmls(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm);
  void vmls(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    vmls(al, dt, rd, rn, rm);
  }

  void vmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vmls(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vmls(al, dt, rd, rn, rm);
  }

  void vmls(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vmls(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vmls(al, dt, rd, rn, rm);
  }

  void vmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vmls(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vmls(al, dt, rd, rn, rm);
  }

  void vmlsl(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm);
  void vmlsl(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    vmlsl(al, dt, rd, rn, rm);
  }

  void vmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vmlsl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vmlsl(al, dt, rd, rn, rm);
  }

  void vmov(Condition cond, Register rt, SRegister rn);
  void vmov(Register rt, SRegister rn) { vmov(al, rt, rn); }

  void vmov(Condition cond, SRegister rn, Register rt);
  void vmov(SRegister rn, Register rt) { vmov(al, rn, rt); }

  void vmov(Condition cond, Register rt, Register rt2, DRegister rm);
  void vmov(Register rt, Register rt2, DRegister rm) { vmov(al, rt, rt2, rm); }

  void vmov(Condition cond, DRegister rm, Register rt, Register rt2);
  void vmov(DRegister rm, Register rt, Register rt2) { vmov(al, rm, rt, rt2); }

  void vmov(
      Condition cond, Register rt, Register rt2, SRegister rm, SRegister rm1);
  void vmov(Register rt, Register rt2, SRegister rm, SRegister rm1) {
    vmov(al, rt, rt2, rm, rm1);
  }

  void vmov(
      Condition cond, SRegister rm, SRegister rm1, Register rt, Register rt2);
  void vmov(SRegister rm, SRegister rm1, Register rt, Register rt2) {
    vmov(al, rm, rm1, rt, rt2);
  }

  void vmov(Condition cond, DataType dt, DRegisterLane rd, Register rt);
  void vmov(DataType dt, DRegisterLane rd, Register rt) {
    vmov(al, dt, rd, rt);
  }
  void vmov(DRegisterLane rd, Register rt) {
    vmov(al, kDataTypeValueNone, rd, rt);
  }
  void vmov(Condition cond, DRegisterLane rd, Register rt) {
    vmov(cond, kDataTypeValueNone, rd, rt);
  }

  void vmov(Condition cond, DataType dt, DRegister rd, const DOperand& operand);
  void vmov(DataType dt, DRegister rd, const DOperand& operand) {
    vmov(al, dt, rd, operand);
  }

  void vmov(Condition cond, DataType dt, QRegister rd, const QOperand& operand);
  void vmov(DataType dt, QRegister rd, const QOperand& operand) {
    vmov(al, dt, rd, operand);
  }

  void vmov(Condition cond, DataType dt, SRegister rd, const SOperand& operand);
  void vmov(DataType dt, SRegister rd, const SOperand& operand) {
    vmov(al, dt, rd, operand);
  }

  void vmov(Condition cond, DataType dt, Register rt, DRegisterLane rn);
  void vmov(DataType dt, Register rt, DRegisterLane rn) {
    vmov(al, dt, rt, rn);
  }
  void vmov(Register rt, DRegisterLane rn) {
    vmov(al, kDataTypeValueNone, rt, rn);
  }
  void vmov(Condition cond, Register rt, DRegisterLane rn) {
    vmov(cond, kDataTypeValueNone, rt, rn);
  }

  void vmovl(Condition cond, DataType dt, QRegister rd, DRegister rm);
  void vmovl(DataType dt, QRegister rd, DRegister rm) { vmovl(al, dt, rd, rm); }

  void vmovn(Condition cond, DataType dt, DRegister rd, QRegister rm);
  void vmovn(DataType dt, DRegister rd, QRegister rm) { vmovn(al, dt, rd, rm); }

  void vmrs(Condition cond, RegisterOrAPSR_nzcv rt, SpecialFPRegister spec_reg);
  void vmrs(RegisterOrAPSR_nzcv rt, SpecialFPRegister spec_reg) {
    vmrs(al, rt, spec_reg);
  }

  void vmsr(Condition cond, SpecialFPRegister spec_reg, Register rt);
  void vmsr(SpecialFPRegister spec_reg, Register rt) { vmsr(al, spec_reg, rt); }

  void vmul(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister dm,
            unsigned index);
  void vmul(
      DataType dt, DRegister rd, DRegister rn, DRegister dm, unsigned index) {
    vmul(al, dt, rd, rn, dm, index);
  }

  void vmul(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegister dm,
            unsigned index);
  void vmul(
      DataType dt, QRegister rd, QRegister rn, DRegister dm, unsigned index) {
    vmul(al, dt, rd, rn, dm, index);
  }

  void vmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vmul(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vmul(al, dt, rd, rn, rm);
  }

  void vmul(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vmul(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vmul(al, dt, rd, rn, rm);
  }

  void vmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vmul(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vmul(al, dt, rd, rn, rm);
  }

  void vmull(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegister dm,
             unsigned index);
  void vmull(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    vmull(al, dt, rd, rn, dm, index);
  }

  void vmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vmull(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vmull(al, dt, rd, rn, rm);
  }

  void vmvn(Condition cond, DataType dt, DRegister rd, const DOperand& operand);
  void vmvn(DataType dt, DRegister rd, const DOperand& operand) {
    vmvn(al, dt, rd, operand);
  }

  void vmvn(Condition cond, DataType dt, QRegister rd, const QOperand& operand);
  void vmvn(DataType dt, QRegister rd, const QOperand& operand) {
    vmvn(al, dt, rd, operand);
  }

  void vneg(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vneg(DataType dt, DRegister rd, DRegister rm) { vneg(al, dt, rd, rm); }

  void vneg(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vneg(DataType dt, QRegister rd, QRegister rm) { vneg(al, dt, rd, rm); }

  void vneg(Condition cond, DataType dt, SRegister rd, SRegister rm);
  void vneg(DataType dt, SRegister rd, SRegister rm) { vneg(al, dt, rd, rm); }

  void vnmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vnmla(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vnmla(al, dt, rd, rn, rm);
  }

  void vnmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vnmla(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vnmla(al, dt, rd, rn, rm);
  }

  void vnmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vnmls(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vnmls(al, dt, rd, rn, rm);
  }

  void vnmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vnmls(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vnmls(al, dt, rd, rn, rm);
  }

  void vnmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vnmul(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vnmul(al, dt, rd, rn, rm);
  }

  void vnmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vnmul(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vnmul(al, dt, rd, rn, rm);
  }

  void vorn(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);
  void vorn(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    vorn(al, dt, rd, rn, operand);
  }

  void vorn(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);
  void vorn(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    vorn(al, dt, rd, rn, operand);
  }

  void vorr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);
  void vorr(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    vorr(al, dt, rd, rn, operand);
  }
  void vorr(DRegister rd, DRegister rn, const DOperand& operand) {
    vorr(al, kDataTypeValueNone, rd, rn, operand);
  }
  void vorr(Condition cond,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    vorr(cond, kDataTypeValueNone, rd, rn, operand);
  }

  void vorr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);
  void vorr(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    vorr(al, dt, rd, rn, operand);
  }
  void vorr(QRegister rd, QRegister rn, const QOperand& operand) {
    vorr(al, kDataTypeValueNone, rd, rn, operand);
  }
  void vorr(Condition cond,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    vorr(cond, kDataTypeValueNone, rd, rn, operand);
  }

  void vpadal(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vpadal(DataType dt, DRegister rd, DRegister rm) {
    vpadal(al, dt, rd, rm);
  }

  void vpadal(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vpadal(DataType dt, QRegister rd, QRegister rm) {
    vpadal(al, dt, rd, rm);
  }

  void vpadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vpadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vpadd(al, dt, rd, rn, rm);
  }

  void vpaddl(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vpaddl(DataType dt, DRegister rd, DRegister rm) {
    vpaddl(al, dt, rd, rm);
  }

  void vpaddl(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vpaddl(DataType dt, QRegister rd, QRegister rm) {
    vpaddl(al, dt, rd, rm);
  }

  void vpmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vpmax(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vpmax(al, dt, rd, rn, rm);
  }

  void vpmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vpmin(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vpmin(al, dt, rd, rn, rm);
  }

  void vpop(Condition cond, DataType dt, DRegisterList dreglist);
  void vpop(DataType dt, DRegisterList dreglist) { vpop(al, dt, dreglist); }
  void vpop(DRegisterList dreglist) { vpop(al, kDataTypeValueNone, dreglist); }
  void vpop(Condition cond, DRegisterList dreglist) {
    vpop(cond, kDataTypeValueNone, dreglist);
  }

  void vpop(Condition cond, DataType dt, SRegisterList sreglist);
  void vpop(DataType dt, SRegisterList sreglist) { vpop(al, dt, sreglist); }
  void vpop(SRegisterList sreglist) { vpop(al, kDataTypeValueNone, sreglist); }
  void vpop(Condition cond, SRegisterList sreglist) {
    vpop(cond, kDataTypeValueNone, sreglist);
  }

  void vpush(Condition cond, DataType dt, DRegisterList dreglist);
  void vpush(DataType dt, DRegisterList dreglist) { vpush(al, dt, dreglist); }
  void vpush(DRegisterList dreglist) {
    vpush(al, kDataTypeValueNone, dreglist);
  }
  void vpush(Condition cond, DRegisterList dreglist) {
    vpush(cond, kDataTypeValueNone, dreglist);
  }

  void vpush(Condition cond, DataType dt, SRegisterList sreglist);
  void vpush(DataType dt, SRegisterList sreglist) { vpush(al, dt, sreglist); }
  void vpush(SRegisterList sreglist) {
    vpush(al, kDataTypeValueNone, sreglist);
  }
  void vpush(Condition cond, SRegisterList sreglist) {
    vpush(cond, kDataTypeValueNone, sreglist);
  }

  void vqabs(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vqabs(DataType dt, DRegister rd, DRegister rm) { vqabs(al, dt, rd, rm); }

  void vqabs(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vqabs(DataType dt, QRegister rd, QRegister rm) { vqabs(al, dt, rd, rm); }

  void vqadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vqadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vqadd(al, dt, rd, rn, rm);
  }

  void vqadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vqadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vqadd(al, dt, rd, rn, rm);
  }

  void vqdmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vqdmlal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vqdmlal(al, dt, rd, rn, rm);
  }

  void vqdmlal(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index);
  void vqdmlal(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    vqdmlal(al, dt, rd, rn, dm, index);
  }

  void vqdmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vqdmlsl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vqdmlsl(al, dt, rd, rn, rm);
  }

  void vqdmlsl(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index);
  void vqdmlsl(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    vqdmlsl(al, dt, rd, rn, dm, index);
  }

  void vqdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vqdmulh(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vqdmulh(al, dt, rd, rn, rm);
  }

  void vqdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vqdmulh(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vqdmulh(al, dt, rd, rn, rm);
  }

  void vqdmulh(Condition cond,
               DataType dt,
               DRegister rd,
               DRegister rn,
               DRegisterLane rm);
  void vqdmulh(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    vqdmulh(al, dt, rd, rn, rm);
  }

  void vqdmulh(Condition cond,
               DataType dt,
               QRegister rd,
               QRegister rn,
               DRegisterLane rm);
  void vqdmulh(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    vqdmulh(al, dt, rd, rn, rm);
  }

  void vqdmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vqdmull(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vqdmull(al, dt, rd, rn, rm);
  }

  void vqdmull(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegisterLane rm);
  void vqdmull(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    vqdmull(al, dt, rd, rn, rm);
  }

  void vqmovn(Condition cond, DataType dt, DRegister rd, QRegister rm);
  void vqmovn(DataType dt, DRegister rd, QRegister rm) {
    vqmovn(al, dt, rd, rm);
  }

  void vqmovun(Condition cond, DataType dt, DRegister rd, QRegister rm);
  void vqmovun(DataType dt, DRegister rd, QRegister rm) {
    vqmovun(al, dt, rd, rm);
  }

  void vqneg(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vqneg(DataType dt, DRegister rd, DRegister rm) { vqneg(al, dt, rd, rm); }

  void vqneg(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vqneg(DataType dt, QRegister rd, QRegister rm) { vqneg(al, dt, rd, rm); }

  void vqrdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vqrdmulh(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vqrdmulh(al, dt, rd, rn, rm);
  }

  void vqrdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vqrdmulh(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vqrdmulh(al, dt, rd, rn, rm);
  }

  void vqrdmulh(Condition cond,
                DataType dt,
                DRegister rd,
                DRegister rn,
                DRegisterLane rm);
  void vqrdmulh(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    vqrdmulh(al, dt, rd, rn, rm);
  }

  void vqrdmulh(Condition cond,
                DataType dt,
                QRegister rd,
                QRegister rn,
                DRegisterLane rm);
  void vqrdmulh(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    vqrdmulh(al, dt, rd, rn, rm);
  }

  void vqrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn);
  void vqrshl(DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    vqrshl(al, dt, rd, rm, rn);
  }

  void vqrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn);
  void vqrshl(DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    vqrshl(al, dt, rd, rm, rn);
  }

  void vqrshrn(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand);
  void vqrshrn(DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    vqrshrn(al, dt, rd, rm, operand);
  }

  void vqrshrun(Condition cond,
                DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand);
  void vqrshrun(DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand) {
    vqrshrun(al, dt, rd, rm, operand);
  }

  void vqshl(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand);
  void vqshl(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vqshl(al, dt, rd, rm, operand);
  }

  void vqshl(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand);
  void vqshl(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vqshl(al, dt, rd, rm, operand);
  }

  void vqshlu(Condition cond,
              DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand);
  void vqshlu(DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand) {
    vqshlu(al, dt, rd, rm, operand);
  }

  void vqshlu(Condition cond,
              DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand);
  void vqshlu(DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand) {
    vqshlu(al, dt, rd, rm, operand);
  }

  void vqshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand);
  void vqshrn(DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    vqshrn(al, dt, rd, rm, operand);
  }

  void vqshrun(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand);
  void vqshrun(DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    vqshrun(al, dt, rd, rm, operand);
  }

  void vqsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vqsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vqsub(al, dt, rd, rn, rm);
  }

  void vqsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vqsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vqsub(al, dt, rd, rn, rm);
  }

  void vraddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);
  void vraddhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    vraddhn(al, dt, rd, rn, rm);
  }

  void vrecpe(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrecpe(DataType dt, DRegister rd, DRegister rm) {
    vrecpe(al, dt, rd, rm);
  }

  void vrecpe(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vrecpe(DataType dt, QRegister rd, QRegister rm) {
    vrecpe(al, dt, rd, rm);
  }

  void vrecps(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vrecps(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vrecps(al, dt, rd, rn, rm);
  }

  void vrecps(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vrecps(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vrecps(al, dt, rd, rn, rm);
  }

  void vrev16(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrev16(DataType dt, DRegister rd, DRegister rm) {
    vrev16(al, dt, rd, rm);
  }

  void vrev16(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vrev16(DataType dt, QRegister rd, QRegister rm) {
    vrev16(al, dt, rd, rm);
  }

  void vrev32(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrev32(DataType dt, DRegister rd, DRegister rm) {
    vrev32(al, dt, rd, rm);
  }

  void vrev32(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vrev32(DataType dt, QRegister rd, QRegister rm) {
    vrev32(al, dt, rd, rm);
  }

  void vrev64(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrev64(DataType dt, DRegister rd, DRegister rm) {
    vrev64(al, dt, rd, rm);
  }

  void vrev64(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vrev64(DataType dt, QRegister rd, QRegister rm) {
    vrev64(al, dt, rd, rm);
  }

  void vrhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vrhadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vrhadd(al, dt, rd, rn, rm);
  }

  void vrhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vrhadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vrhadd(al, dt, rd, rn, rm);
  }

  void vrinta(DataType dt, DRegister rd, DRegister rm);

  void vrinta(DataType dt, QRegister rd, QRegister rm);

  void vrinta(DataType dt, SRegister rd, SRegister rm);

  void vrintm(DataType dt, DRegister rd, DRegister rm);

  void vrintm(DataType dt, QRegister rd, QRegister rm);

  void vrintm(DataType dt, SRegister rd, SRegister rm);

  void vrintn(DataType dt, DRegister rd, DRegister rm);

  void vrintn(DataType dt, QRegister rd, QRegister rm);

  void vrintn(DataType dt, SRegister rd, SRegister rm);

  void vrintp(DataType dt, DRegister rd, DRegister rm);

  void vrintp(DataType dt, QRegister rd, QRegister rm);

  void vrintp(DataType dt, SRegister rd, SRegister rm);

  void vrintr(Condition cond, DataType dt, SRegister rd, SRegister rm);
  void vrintr(DataType dt, SRegister rd, SRegister rm) {
    vrintr(al, dt, rd, rm);
  }

  void vrintr(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrintr(DataType dt, DRegister rd, DRegister rm) {
    vrintr(al, dt, rd, rm);
  }

  void vrintx(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrintx(DataType dt, DRegister rd, DRegister rm) {
    vrintx(al, dt, rd, rm);
  }

  void vrintx(DataType dt, QRegister rd, QRegister rm);

  void vrintx(Condition cond, DataType dt, SRegister rd, SRegister rm);
  void vrintx(DataType dt, SRegister rd, SRegister rm) {
    vrintx(al, dt, rd, rm);
  }

  void vrintz(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrintz(DataType dt, DRegister rd, DRegister rm) {
    vrintz(al, dt, rd, rm);
  }

  void vrintz(DataType dt, QRegister rd, QRegister rm);

  void vrintz(Condition cond, DataType dt, SRegister rd, SRegister rm);
  void vrintz(DataType dt, SRegister rd, SRegister rm) {
    vrintz(al, dt, rd, rm);
  }

  void vrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn);
  void vrshl(DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    vrshl(al, dt, rd, rm, rn);
  }

  void vrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn);
  void vrshl(DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    vrshl(al, dt, rd, rm, rn);
  }

  void vrshr(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand);
  void vrshr(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vrshr(al, dt, rd, rm, operand);
  }

  void vrshr(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand);
  void vrshr(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vrshr(al, dt, rd, rm, operand);
  }

  void vrshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand);
  void vrshrn(DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    vrshrn(al, dt, rd, rm, operand);
  }

  void vrsqrte(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vrsqrte(DataType dt, DRegister rd, DRegister rm) {
    vrsqrte(al, dt, rd, rm);
  }

  void vrsqrte(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vrsqrte(DataType dt, QRegister rd, QRegister rm) {
    vrsqrte(al, dt, rd, rm);
  }

  void vrsqrts(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vrsqrts(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vrsqrts(al, dt, rd, rn, rm);
  }

  void vrsqrts(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vrsqrts(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vrsqrts(al, dt, rd, rn, rm);
  }

  void vrsra(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand);
  void vrsra(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vrsra(al, dt, rd, rm, operand);
  }

  void vrsra(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand);
  void vrsra(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vrsra(al, dt, rd, rm, operand);
  }

  void vrsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);
  void vrsubhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    vrsubhn(al, dt, rd, rn, rm);
  }

  void vseleq(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vseleq(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vselge(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vselge(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vselgt(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vselgt(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vselvs(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vselvs(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vshl(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vshl(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vshl(al, dt, rd, rm, operand);
  }

  void vshl(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vshl(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vshl(al, dt, rd, rm, operand);
  }

  void vshll(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rm,
             const DOperand& operand);
  void vshll(DataType dt, QRegister rd, DRegister rm, const DOperand& operand) {
    vshll(al, dt, rd, rm, operand);
  }

  void vshr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vshr(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vshr(al, dt, rd, rm, operand);
  }

  void vshr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vshr(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vshr(al, dt, rd, rm, operand);
  }

  void vshrn(Condition cond,
             DataType dt,
             DRegister rd,
             QRegister rm,
             const QOperand& operand);
  void vshrn(DataType dt, DRegister rd, QRegister rm, const QOperand& operand) {
    vshrn(al, dt, rd, rm, operand);
  }

  void vsli(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vsli(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vsli(al, dt, rd, rm, operand);
  }

  void vsli(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vsli(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vsli(al, dt, rd, rm, operand);
  }

  void vsqrt(Condition cond, DataType dt, SRegister rd, SRegister rm);
  void vsqrt(DataType dt, SRegister rd, SRegister rm) { vsqrt(al, dt, rd, rm); }

  void vsqrt(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vsqrt(DataType dt, DRegister rd, DRegister rm) { vsqrt(al, dt, rd, rm); }

  void vsra(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vsra(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vsra(al, dt, rd, rm, operand);
  }

  void vsra(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vsra(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vsra(al, dt, rd, rm, operand);
  }

  void vsri(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);
  void vsri(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    vsri(al, dt, rd, rm, operand);
  }

  void vsri(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);
  void vsri(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    vsri(al, dt, rd, rm, operand);
  }

  void vst1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vst1(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vst1(al, dt, nreglist, operand);
  }

  void vst2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vst2(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vst2(al, dt, nreglist, operand);
  }

  void vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vst3(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vst3(al, dt, nreglist, operand);
  }

  void vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand);
  void vst3(DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    vst3(al, dt, nreglist, operand);
  }

  void vst4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);
  void vst4(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    vst4(al, dt, nreglist, operand);
  }

  void vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist);
  void vstm(DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    vstm(al, dt, rn, write_back, dreglist);
  }
  void vstm(Register rn, WriteBack write_back, DRegisterList dreglist) {
    vstm(al, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void vstm(Condition cond,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    vstm(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist);
  void vstm(DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    vstm(al, dt, rn, write_back, sreglist);
  }
  void vstm(Register rn, WriteBack write_back, SRegisterList sreglist) {
    vstm(al, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void vstm(Condition cond,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    vstm(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);
  void vstmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vstmdb(al, dt, rn, write_back, dreglist);
  }
  void vstmdb(Register rn, WriteBack write_back, DRegisterList dreglist) {
    vstmdb(al, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void vstmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vstmdb(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);
  void vstmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vstmdb(al, dt, rn, write_back, sreglist);
  }
  void vstmdb(Register rn, WriteBack write_back, SRegisterList sreglist) {
    vstmdb(al, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void vstmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vstmdb(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);
  void vstmia(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vstmia(al, dt, rn, write_back, dreglist);
  }
  void vstmia(Register rn, WriteBack write_back, DRegisterList dreglist) {
    vstmia(al, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void vstmia(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    vstmia(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);
  void vstmia(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vstmia(al, dt, rn, write_back, sreglist);
  }
  void vstmia(Register rn, WriteBack write_back, SRegisterList sreglist) {
    vstmia(al, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void vstmia(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    vstmia(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void vstr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand);
  void vstr(DataType dt, DRegister rd, const MemOperand& operand) {
    vstr(al, dt, rd, operand);
  }
  void vstr(DRegister rd, const MemOperand& operand) {
    vstr(al, Untyped64, rd, operand);
  }
  void vstr(Condition cond, DRegister rd, const MemOperand& operand) {
    vstr(cond, Untyped64, rd, operand);
  }

  void vstr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand);
  void vstr(DataType dt, SRegister rd, const MemOperand& operand) {
    vstr(al, dt, rd, operand);
  }
  void vstr(SRegister rd, const MemOperand& operand) {
    vstr(al, Untyped32, rd, operand);
  }
  void vstr(Condition cond, SRegister rd, const MemOperand& operand) {
    vstr(cond, Untyped32, rd, operand);
  }

  void vsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vsub(al, dt, rd, rn, rm);
  }

  void vsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vsub(al, dt, rd, rn, rm);
  }

  void vsub(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);
  void vsub(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    vsub(al, dt, rd, rn, rm);
  }

  void vsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);
  void vsubhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    vsubhn(al, dt, rd, rn, rm);
  }

  void vsubl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);
  void vsubl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    vsubl(al, dt, rd, rn, rm);
  }

  void vsubw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm);
  void vsubw(DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    vsubw(al, dt, rd, rn, rm);
  }

  void vswp(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vswp(DataType dt, DRegister rd, DRegister rm) { vswp(al, dt, rd, rm); }
  void vswp(DRegister rd, DRegister rm) {
    vswp(al, kDataTypeValueNone, rd, rm);
  }
  void vswp(Condition cond, DRegister rd, DRegister rm) {
    vswp(cond, kDataTypeValueNone, rd, rm);
  }

  void vswp(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vswp(DataType dt, QRegister rd, QRegister rm) { vswp(al, dt, rd, rm); }
  void vswp(QRegister rd, QRegister rm) {
    vswp(al, kDataTypeValueNone, rd, rm);
  }
  void vswp(Condition cond, QRegister rd, QRegister rm) {
    vswp(cond, kDataTypeValueNone, rd, rm);
  }

  void vtbl(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm);
  void vtbl(DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    vtbl(al, dt, rd, nreglist, rm);
  }

  void vtbx(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm);
  void vtbx(DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    vtbx(al, dt, rd, nreglist, rm);
  }

  void vtrn(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vtrn(DataType dt, DRegister rd, DRegister rm) { vtrn(al, dt, rd, rm); }

  void vtrn(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vtrn(DataType dt, QRegister rd, QRegister rm) { vtrn(al, dt, rd, rm); }

  void vtst(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);
  void vtst(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    vtst(al, dt, rd, rn, rm);
  }

  void vtst(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);
  void vtst(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    vtst(al, dt, rd, rn, rm);
  }

  void vuzp(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vuzp(DataType dt, DRegister rd, DRegister rm) { vuzp(al, dt, rd, rm); }

  void vuzp(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vuzp(DataType dt, QRegister rd, QRegister rm) { vuzp(al, dt, rd, rm); }

  void vzip(Condition cond, DataType dt, DRegister rd, DRegister rm);
  void vzip(DataType dt, DRegister rd, DRegister rm) { vzip(al, dt, rd, rm); }

  void vzip(Condition cond, DataType dt, QRegister rd, QRegister rm);
  void vzip(DataType dt, QRegister rd, QRegister rm) { vzip(al, dt, rd, rm); }

  void yield(Condition cond, EncodingSize size);
  void yield() { yield(al, Best); }
  void yield(Condition cond) { yield(cond, Best); }
  void yield(EncodingSize size) { yield(al, size); }
  // End of generated code.
  virtual void UnimplementedDelegate(InstructionType type) {
    std::string error_message(std::string("Ill-formed '") +
                              std::string(ToCString(type)) +
                              std::string("' instruction.\n"));
    VIXL_ABORT_WITH_MSG(error_message.c_str());
  }
  virtual bool AllowUnpredictable() { return allow_unpredictable_; }
  virtual bool AllowStronglyDiscouraged() {
    return allow_strongly_discouraged_;
  }
};

}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_AARCH32_ASSEMBLER_AARCH32_H_
