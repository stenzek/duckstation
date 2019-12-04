// Copyright 2017, VIXL authors
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

#ifndef VIXL_AARCH32_MACRO_ASSEMBLER_AARCH32_H_
#define VIXL_AARCH32_MACRO_ASSEMBLER_AARCH32_H_

#include "code-generation-scopes-vixl.h"
#include "macro-assembler-interface.h"
#include "pool-manager-impl.h"
#include "pool-manager.h"
#include "utils-vixl.h"

#include "aarch32/assembler-aarch32.h"
#include "aarch32/instructions-aarch32.h"
#include "aarch32/operands-aarch32.h"

namespace vixl {

namespace aarch32 {

class UseScratchRegisterScope;

enum FlagsUpdate { LeaveFlags = 0, SetFlags = 1, DontCare = 2 };

// We use a subclass to access the protected `ExactAssemblyScope` constructor
// giving us control over the pools, and make the constructor private to limit
// usage to code paths emitting pools.
class ExactAssemblyScopeWithoutPoolsCheck : public ExactAssemblyScope {
 private:
  ExactAssemblyScopeWithoutPoolsCheck(MacroAssembler* masm,
                                      size_t size,
                                      SizePolicy size_policy = kExactSize);

  friend class MacroAssembler;
  friend class Label;
};
// Macro assembler for aarch32 instruction set.
class MacroAssembler : public Assembler, public MacroAssemblerInterface {
 public:
  enum FinalizeOption {
    kFallThrough,  // There may be more code to execute after calling Finalize.
    kUnreachable   // Anything generated after calling Finalize is unreachable.
  };

  virtual internal::AssemblerBase* AsAssemblerBase() VIXL_OVERRIDE {
    return this;
  }

  virtual bool ArePoolsBlocked() const VIXL_OVERRIDE {
    return pool_manager_.IsBlocked();
  }

  virtual void EmitPoolHeader() VIXL_OVERRIDE {
    // Check that we have the correct alignment.
    if (IsUsingT32()) {
      VIXL_ASSERT(GetBuffer()->Is16bitAligned());
    } else {
      VIXL_ASSERT(GetBuffer()->Is32bitAligned());
    }
    VIXL_ASSERT(pool_end_ == NULL);
    pool_end_ = new Label();
    ExactAssemblyScopeWithoutPoolsCheck guard(this,
                                              kMaxInstructionSizeInBytes,
                                              ExactAssemblyScope::kMaximumSize);
    b(pool_end_);
  }
  virtual void EmitPoolFooter() VIXL_OVERRIDE {
    // Align buffer to 4 bytes.
    GetBuffer()->Align();
    if (pool_end_ != NULL) {
      Bind(pool_end_);
      delete pool_end_;
      pool_end_ = NULL;
    }
  }
  virtual void EmitPaddingBytes(int n) VIXL_OVERRIDE {
    GetBuffer()->EmitZeroedBytes(n);
  }
  virtual void EmitNopBytes(int n) VIXL_OVERRIDE {
    int nops = 0;
    int nop_size = IsUsingT32() ? k16BitT32InstructionSizeInBytes
                                : kA32InstructionSizeInBytes;
    VIXL_ASSERT(n % nop_size == 0);
    nops = n / nop_size;
    ExactAssemblyScopeWithoutPoolsCheck guard(this,
                                              n,
                                              ExactAssemblyScope::kExactSize);
    for (int i = 0; i < nops; ++i) {
      nop();
    }
  }


 private:
  class MacroEmissionCheckScope : public EmissionCheckScope {
   public:
    explicit MacroEmissionCheckScope(MacroAssemblerInterface* masm,
                                     PoolPolicy pool_policy = kBlockPools)
        : EmissionCheckScope(masm,
                             kTypicalMacroInstructionMaxSize,
                             kMaximumSize,
                             pool_policy) {}

   private:
    static const size_t kTypicalMacroInstructionMaxSize =
        8 * kMaxInstructionSizeInBytes;
  };

  class MacroAssemblerContext {
   public:
    MacroAssemblerContext() : count_(0) {}
    ~MacroAssemblerContext() {}
    unsigned GetRecursiveCount() const { return count_; }
    void Up(const char* loc) {
      location_stack_[count_] = loc;
      count_++;
      if (count_ >= kMaxRecursion) {
        printf(
            "Recursion limit reached; unable to resolve macro assembler "
            "call.\n");
        printf("Macro assembler context stack:\n");
        for (unsigned i = 0; i < kMaxRecursion; i++) {
          printf("%10s %s\n", (i == 0) ? "oldest -> " : "", location_stack_[i]);
        }
        VIXL_ABORT();
      }
    }
    void Down() {
      VIXL_ASSERT((count_ > 0) && (count_ < kMaxRecursion));
      count_--;
    }

   private:
    unsigned count_;
    static const uint32_t kMaxRecursion = 6;
    const char* location_stack_[kMaxRecursion];
  };

  // This scope is used at each Delegate entry to avoid infinite recursion of
  // Delegate calls. The limit is defined by
  // MacroAssemblerContext::kMaxRecursion.
  class ContextScope {
   public:
    explicit ContextScope(MacroAssembler* const masm, const char* loc)
        : masm_(masm) {
      VIXL_ASSERT(masm_->AllowMacroInstructions());
      masm_->GetContext()->Up(loc);
    }
    ~ContextScope() { masm_->GetContext()->Down(); }

   private:
    MacroAssembler* const masm_;
  };

  MacroAssemblerContext* GetContext() { return &context_; }

  class ITScope {
   public:
    ITScope(MacroAssembler* masm,
            Condition* cond,
            const MacroEmissionCheckScope& scope,
            bool can_use_it = false)
        : masm_(masm), cond_(*cond), can_use_it_(can_use_it) {
      // The 'scope' argument is used to remind us to only use this scope inside
      // a MacroEmissionCheckScope. This way, we do not need to check whether
      // we need to emit the pools or grow the code buffer when emitting the
      // IT or B instructions.
      USE(scope);
      if (!cond_.Is(al) && masm->IsUsingT32()) {
        if (can_use_it_) {
          // IT is not deprecated (that implies a 16 bit T32 instruction).
          // We generate an IT instruction and a conditional instruction.
          masm->it(cond_);
        } else {
          // The usage of IT is deprecated for the instruction.
          // We generate a conditional branch and an unconditional instruction.
          // Generate the branch.
          masm_->b(cond_.Negate(), Narrow, &label_);
          // Tell the macro-assembler to generate unconditional instructions.
          *cond = al;
        }
      }
#ifdef VIXL_DEBUG
      initial_cursor_offset_ = masm->GetCursorOffset();
#else
      USE(initial_cursor_offset_);
#endif
    }
    ~ITScope() {
      if (label_.IsReferenced()) {
        // We only use the label for conditional T32 instructions for which we
        // cannot use IT.
        VIXL_ASSERT(!cond_.Is(al));
        VIXL_ASSERT(masm_->IsUsingT32());
        VIXL_ASSERT(!can_use_it_);
        VIXL_ASSERT(masm_->GetCursorOffset() - initial_cursor_offset_ <=
                    kMaxT32MacroInstructionSizeInBytes);
        masm_->BindHelper(&label_);
      } else if (masm_->IsUsingT32() && !cond_.Is(al)) {
        // If we've generated a conditional T32 instruction but haven't used the
        // label, we must have used IT. Check that we did not generate a
        // deprecated sequence.
        VIXL_ASSERT(can_use_it_);
        VIXL_ASSERT(masm_->GetCursorOffset() - initial_cursor_offset_ <=
                    k16BitT32InstructionSizeInBytes);
      }
    }

   private:
    MacroAssembler* masm_;
    Condition cond_;
    Label label_;
    bool can_use_it_;
    uint32_t initial_cursor_offset_;
  };

 protected:
  virtual void BlockPools() VIXL_OVERRIDE { pool_manager_.Block(); }
  virtual void ReleasePools() VIXL_OVERRIDE {
    pool_manager_.Release(GetCursorOffset());
  }
  virtual void EnsureEmitPoolsFor(size_t size) VIXL_OVERRIDE;

  // Tell whether any of the macro instruction can be used. When false the
  // MacroAssembler will assert if a method which can emit a variable number
  // of instructions is called.
  virtual void SetAllowMacroInstructions(bool value) VIXL_OVERRIDE {
    allow_macro_instructions_ = value;
  }

  void HandleOutOfBoundsImmediate(Condition cond, Register tmp, uint32_t imm);

 public:
  // TODO: If we change the MacroAssembler to disallow setting a different ISA,
  // we can change the alignment of the pool in the pool manager constructor to
  // be 2 bytes for T32.
  explicit MacroAssembler(InstructionSet isa = kDefaultISA)
      : Assembler(isa),
        available_(r12),
        current_scratch_scope_(NULL),
        pool_manager_(4 /*header_size*/,
                      4 /*alignment*/,
                      4 /*buffer_alignment*/),
        generate_simulator_code_(VIXL_AARCH32_GENERATE_SIMULATOR_CODE),
        pool_end_(NULL) {
#ifdef VIXL_DEBUG
    SetAllowMacroInstructions(true);
#else
    USE(allow_macro_instructions_);
#endif
  }
  explicit MacroAssembler(size_t size, InstructionSet isa = kDefaultISA)
      : Assembler(size, isa),
        available_(r12),
        current_scratch_scope_(NULL),
        pool_manager_(4 /*header_size*/,
                      4 /*alignment*/,
                      4 /*buffer_alignment*/),
        generate_simulator_code_(VIXL_AARCH32_GENERATE_SIMULATOR_CODE),
        pool_end_(NULL) {
#ifdef VIXL_DEBUG
    SetAllowMacroInstructions(true);
#endif
  }
  MacroAssembler(byte* buffer, size_t size, InstructionSet isa = kDefaultISA)
      : Assembler(buffer, size, isa),
        available_(r12),
        current_scratch_scope_(NULL),
        pool_manager_(4 /*header_size*/,
                      4 /*alignment*/,
                      4 /*buffer_alignment*/),
        generate_simulator_code_(VIXL_AARCH32_GENERATE_SIMULATOR_CODE),
        pool_end_(NULL) {
#ifdef VIXL_DEBUG
    SetAllowMacroInstructions(true);
#endif
  }

  bool GenerateSimulatorCode() const { return generate_simulator_code_; }

  virtual bool AllowMacroInstructions() const VIXL_OVERRIDE {
    return allow_macro_instructions_;
  }

  void FinalizeCode(FinalizeOption option = kUnreachable) {
    EmitLiteralPool(option == kUnreachable
                        ? PoolManager<int32_t>::kNoBranchRequired
                        : PoolManager<int32_t>::kBranchRequired);
    Assembler::FinalizeCode();
  }

  RegisterList* GetScratchRegisterList() { return &available_; }
  VRegisterList* GetScratchVRegisterList() { return &available_vfp_; }

  // Get or set the current (most-deeply-nested) UseScratchRegisterScope.
  void SetCurrentScratchRegisterScope(UseScratchRegisterScope* scope) {
    current_scratch_scope_ = scope;
  }
  UseScratchRegisterScope* GetCurrentScratchRegisterScope() {
    return current_scratch_scope_;
  }

  // Given an address calculation (Register + immediate), generate code to
  // partially compute the address. The returned MemOperand will perform any
  // remaining computation in a subsequent load or store instruction.
  //
  // The offset provided should be the offset that would be used in a load or
  // store instruction (if it had sufficient range). This only matters where
  // base.Is(pc), since load and store instructions align the pc before
  // dereferencing it.
  //
  // TODO: Improve the handling of negative offsets. They are not implemented
  // precisely for now because they only have a marginal benefit for the
  // existing uses (in delegates).
  MemOperand MemOperandComputationHelper(Condition cond,
                                         Register scratch,
                                         Register base,
                                         uint32_t offset,
                                         uint32_t extra_offset_mask = 0);

  MemOperand MemOperandComputationHelper(Register scratch,
                                         Register base,
                                         uint32_t offset,
                                         uint32_t extra_offset_mask = 0) {
    return MemOperandComputationHelper(al,
                                       scratch,
                                       base,
                                       offset,
                                       extra_offset_mask);
  }
  MemOperand MemOperandComputationHelper(Condition cond,
                                         Register scratch,
                                         Location* location,
                                         uint32_t extra_offset_mask = 0) {
    // Check for buffer space _before_ calculating the offset, in case we
    // generate a pool that affects the offset calculation.
    CodeBufferCheckScope scope(this, 4 * kMaxInstructionSizeInBytes);
    Label::Offset offset =
        location->GetLocation() -
        AlignDown(GetCursorOffset() + GetArchitectureStatePCOffset(), 4);
    return MemOperandComputationHelper(cond,
                                       scratch,
                                       pc,
                                       offset,
                                       extra_offset_mask);
  }
  MemOperand MemOperandComputationHelper(Register scratch,
                                         Location* location,
                                         uint32_t extra_offset_mask = 0) {
    return MemOperandComputationHelper(al,
                                       scratch,
                                       location,
                                       extra_offset_mask);
  }

  // Determine the appropriate mask to pass into MemOperandComputationHelper.
  uint32_t GetOffsetMask(InstructionType type, AddrMode addrmode);

  // State and type helpers.
  bool IsModifiedImmediate(uint32_t imm) {
    return IsUsingT32() ? ImmediateT32::IsImmediateT32(imm)
                        : ImmediateA32::IsImmediateA32(imm);
  }

  void Bind(Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    BindHelper(label);
  }

  virtual void BindHelper(Label* label) VIXL_OVERRIDE {
    // Assert that we have the correct buffer alignment.
    if (IsUsingT32()) {
      VIXL_ASSERT(GetBuffer()->Is16bitAligned());
    } else {
      VIXL_ASSERT(GetBuffer()->Is32bitAligned());
    }
    // If we need to add padding, check if we have to emit the pool.
    const int32_t pc = GetCursorOffset();
    if (label->Needs16BitPadding(pc)) {
      const int kPaddingBytes = 2;
      if (pool_manager_.MustEmit(pc, kPaddingBytes)) {
        int32_t new_pc = pool_manager_.Emit(this, pc, kPaddingBytes);
        USE(new_pc);
        VIXL_ASSERT(new_pc == GetCursorOffset());
      }
    }
    pool_manager_.Bind(this, label, GetCursorOffset());
  }

  void RegisterLiteralReference(RawLiteral* literal) {
    if (literal->IsManuallyPlaced()) return;
    RegisterForwardReference(literal);
  }

  void RegisterForwardReference(Location* location) {
    if (location->IsBound()) return;
    VIXL_ASSERT(location->HasForwardReferences());
    const Location::ForwardRef& reference = location->GetLastForwardReference();
    pool_manager_.AddObjectReference(&reference, location);
  }

  void CheckEmitPoolForInstruction(const ReferenceInfo* info,
                                   Location* location,
                                   Condition* cond = NULL) {
    int size = info->size;
    int32_t pc = GetCursorOffset();
    // If we need to emit a branch over the instruction, take this into account.
    if ((cond != NULL) && NeedBranch(cond)) {
      size += kBranchSize;
      pc += kBranchSize;
    }
    int32_t from = pc;
    from += IsUsingT32() ? kT32PcDelta : kA32PcDelta;
    if (info->pc_needs_aligning) from = AlignDown(from, 4);
    int32_t min = from + info->min_offset;
    int32_t max = from + info->max_offset;
    ForwardReference<int32_t> temp_ref(pc,
                                       info->size,
                                       min,
                                       max,
                                       info->alignment);
    if (pool_manager_.MustEmit(GetCursorOffset(), size, &temp_ref, location)) {
      int32_t new_pc = pool_manager_.Emit(this,
                                          GetCursorOffset(),
                                          info->size,
                                          &temp_ref,
                                          location);
      USE(new_pc);
      VIXL_ASSERT(new_pc == GetCursorOffset());
    }
  }

  void Place(RawLiteral* literal) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(literal->IsManuallyPlaced());
    // Check if we need to emit the pools. Take the alignment of the literal
    // into account, as well as potential 16-bit padding needed to reach the
    // minimum accessible location.
    int alignment = literal->GetMaxAlignment();
    int32_t pc = GetCursorOffset();
    int total_size = AlignUp(pc, alignment) - pc + literal->GetSize();
    if (literal->Needs16BitPadding(pc)) total_size += 2;
    if (pool_manager_.MustEmit(pc, total_size)) {
      int32_t new_pc = pool_manager_.Emit(this, pc, total_size);
      USE(new_pc);
      VIXL_ASSERT(new_pc == GetCursorOffset());
    }
    pool_manager_.Bind(this, literal, GetCursorOffset());
    literal->EmitPoolObject(this);
    // Align the buffer, to be ready to generate instructions right after
    // this.
    GetBuffer()->Align();
  }

  void EmitLiteralPool(PoolManager<int32_t>::EmitOption option =
                           PoolManager<int32_t>::kBranchRequired) {
    VIXL_ASSERT(!ArePoolsBlocked());
    int32_t new_pc =
        pool_manager_.Emit(this, GetCursorOffset(), 0, NULL, NULL, option);
    VIXL_ASSERT(new_pc == GetCursorOffset());
    USE(new_pc);
  }

  void EnsureEmitFor(uint32_t size) {
    EnsureEmitPoolsFor(size);
    VIXL_ASSERT(GetBuffer()->HasSpaceFor(size) || GetBuffer()->IsManaged());
    GetBuffer()->EnsureSpaceFor(size);
  }

  bool AliasesAvailableScratchRegister(Register reg) {
    return GetScratchRegisterList()->Includes(reg);
  }

  bool AliasesAvailableScratchRegister(RegisterOrAPSR_nzcv reg) {
    if (reg.IsAPSR_nzcv()) return false;
    return GetScratchRegisterList()->Includes(reg.AsRegister());
  }

  bool AliasesAvailableScratchRegister(VRegister reg) {
    return GetScratchVRegisterList()->IncludesAliasOf(reg);
  }

  bool AliasesAvailableScratchRegister(const Operand& operand) {
    if (operand.IsImmediate()) return false;
    return AliasesAvailableScratchRegister(operand.GetBaseRegister()) ||
           (operand.IsRegisterShiftedRegister() &&
            AliasesAvailableScratchRegister(operand.GetShiftRegister()));
  }

  bool AliasesAvailableScratchRegister(const NeonOperand& operand) {
    if (operand.IsImmediate()) return false;
    return AliasesAvailableScratchRegister(operand.GetRegister());
  }

  bool AliasesAvailableScratchRegister(SRegisterList list) {
    for (int n = 0; n < list.GetLength(); n++) {
      if (AliasesAvailableScratchRegister(list.GetSRegister(n))) return true;
    }
    return false;
  }

  bool AliasesAvailableScratchRegister(DRegisterList list) {
    for (int n = 0; n < list.GetLength(); n++) {
      if (AliasesAvailableScratchRegister(list.GetDRegister(n))) return true;
    }
    return false;
  }

  bool AliasesAvailableScratchRegister(NeonRegisterList list) {
    for (int n = 0; n < list.GetLength(); n++) {
      if (AliasesAvailableScratchRegister(list.GetDRegister(n))) return true;
    }
    return false;
  }

  bool AliasesAvailableScratchRegister(RegisterList list) {
    return GetScratchRegisterList()->Overlaps(list);
  }

  bool AliasesAvailableScratchRegister(const MemOperand& operand) {
    return AliasesAvailableScratchRegister(operand.GetBaseRegister()) ||
           (operand.IsShiftedRegister() &&
            AliasesAvailableScratchRegister(operand.GetOffsetRegister()));
  }

  // Adr with a literal already constructed. Add the literal to the pool if it
  // is not already done.
  void Adr(Condition cond, Register rd, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = adr_info(cond, Best, rd, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    adr(cond, Best, rd, literal);
    RegisterLiteralReference(literal);
  }
  void Adr(Register rd, RawLiteral* literal) { Adr(al, rd, literal); }

  // Loads with literals already constructed. Add the literal to the pool
  // if it is not already done.
  void Ldr(Condition cond, Register rt, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = ldr_info(cond, Best, rt, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    ldr(cond, rt, literal);
    RegisterLiteralReference(literal);
  }
  void Ldr(Register rt, RawLiteral* literal) { Ldr(al, rt, literal); }

  void Ldrb(Condition cond, Register rt, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = ldrb_info(cond, rt, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    ldrb(cond, rt, literal);
    RegisterLiteralReference(literal);
  }
  void Ldrb(Register rt, RawLiteral* literal) { Ldrb(al, rt, literal); }

  void Ldrd(Condition cond, Register rt, Register rt2, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = ldrd_info(cond, rt, rt2, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    ldrd(cond, rt, rt2, literal);
    RegisterLiteralReference(literal);
  }
  void Ldrd(Register rt, Register rt2, RawLiteral* literal) {
    Ldrd(al, rt, rt2, literal);
  }

  void Ldrh(Condition cond, Register rt, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = ldrh_info(cond, rt, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    ldrh(cond, rt, literal);
    RegisterLiteralReference(literal);
  }
  void Ldrh(Register rt, RawLiteral* literal) { Ldrh(al, rt, literal); }

  void Ldrsb(Condition cond, Register rt, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = ldrsb_info(cond, rt, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    ldrsb(cond, rt, literal);
    RegisterLiteralReference(literal);
  }
  void Ldrsb(Register rt, RawLiteral* literal) { Ldrsb(al, rt, literal); }

  void Ldrsh(Condition cond, Register rt, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = ldrsh_info(cond, rt, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    ldrsh(cond, rt, literal);
    RegisterLiteralReference(literal);
  }
  void Ldrsh(Register rt, RawLiteral* literal) { Ldrsh(al, rt, literal); }

  void Vldr(Condition cond, DataType dt, DRegister rd, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = vldr_info(cond, dt, rd, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    vldr(cond, dt, rd, literal);
    RegisterLiteralReference(literal);
  }
  void Vldr(DataType dt, DRegister rd, RawLiteral* literal) {
    Vldr(al, dt, rd, literal);
  }
  void Vldr(Condition cond, DRegister rd, RawLiteral* literal) {
    Vldr(cond, Untyped64, rd, literal);
  }
  void Vldr(DRegister rd, RawLiteral* literal) {
    Vldr(al, Untyped64, rd, literal);
  }

  void Vldr(Condition cond, DataType dt, SRegister rd, RawLiteral* literal) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!literal->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = vldr_info(cond, dt, rd, literal, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, literal, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    vldr(cond, dt, rd, literal);
    RegisterLiteralReference(literal);
  }
  void Vldr(DataType dt, SRegister rd, RawLiteral* literal) {
    Vldr(al, dt, rd, literal);
  }
  void Vldr(Condition cond, SRegister rd, RawLiteral* literal) {
    Vldr(cond, Untyped32, rd, literal);
  }
  void Vldr(SRegister rd, RawLiteral* literal) {
    Vldr(al, Untyped32, rd, literal);
  }

  // Generic Ldr(register, data)
  void Ldr(Condition cond, Register rt, uint32_t v) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    RawLiteral* literal =
        new Literal<uint32_t>(v, RawLiteral::kDeletedOnPlacementByPool);
    Ldr(cond, rt, literal);
  }
  template <typename T>
  void Ldr(Register rt, T v) {
    Ldr(al, rt, v);
  }

  // Generic Ldrd(rt, rt2, data)
  void Ldrd(Condition cond, Register rt, Register rt2, uint64_t v) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    RawLiteral* literal =
        new Literal<uint64_t>(v, RawLiteral::kDeletedOnPlacementByPool);
    Ldrd(cond, rt, rt2, literal);
  }
  template <typename T>
  void Ldrd(Register rt, Register rt2, T v) {
    Ldrd(al, rt, rt2, v);
  }

  void Vldr(Condition cond, SRegister rd, float v) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    RawLiteral* literal =
        new Literal<float>(v, RawLiteral::kDeletedOnPlacementByPool);
    Vldr(cond, rd, literal);
  }
  void Vldr(SRegister rd, float v) { Vldr(al, rd, v); }

  void Vldr(Condition cond, DRegister rd, double v) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    RawLiteral* literal =
        new Literal<double>(v, RawLiteral::kDeletedOnPlacementByPool);
    Vldr(cond, rd, literal);
  }
  void Vldr(DRegister rd, double v) { Vldr(al, rd, v); }

  void Vmov(Condition cond, DRegister rt, double v) { Vmov(cond, F64, rt, v); }
  void Vmov(DRegister rt, double v) { Vmov(al, F64, rt, v); }
  void Vmov(Condition cond, SRegister rt, float v) { Vmov(cond, F32, rt, v); }
  void Vmov(SRegister rt, float v) { Vmov(al, F32, rt, v); }

  // Claim memory on the stack.
  // Note that the Claim, Drop, and Peek helpers below ensure that offsets used
  // are multiples of 32 bits to help maintain 32-bit SP alignment.
  // We could `Align{Up,Down}(size, 4)`, but that's potentially problematic:
  //     Claim(3)
  //     Claim(1)
  //     Drop(4)
  // would seem correct, when in fact:
  //    Claim(3) -> sp = sp - 4
  //    Claim(1) -> sp = sp - 4
  //    Drop(4)  -> sp = sp + 4
  //
  void Claim(int32_t size) {
    if (size == 0) return;
    // The stack must be kept 32bit aligned.
    VIXL_ASSERT((size > 0) && ((size % 4) == 0));
    Sub(sp, sp, size);
  }
  // Release memory on the stack
  void Drop(int32_t size) {
    if (size == 0) return;
    // The stack must be kept 32bit aligned.
    VIXL_ASSERT((size > 0) && ((size % 4) == 0));
    Add(sp, sp, size);
  }
  void Peek(Register dst, int32_t offset) {
    VIXL_ASSERT((offset >= 0) && ((offset % 4) == 0));
    Ldr(dst, MemOperand(sp, offset));
  }
  void Poke(Register src, int32_t offset) {
    VIXL_ASSERT((offset >= 0) && ((offset % 4) == 0));
    Str(src, MemOperand(sp, offset));
  }
  void Printf(const char* format,
              CPURegister reg1 = NoReg,
              CPURegister reg2 = NoReg,
              CPURegister reg3 = NoReg,
              CPURegister reg4 = NoReg);
  // Functions used by Printf for generation.
  void PushRegister(CPURegister reg);
  void PreparePrintfArgument(CPURegister reg,
                             int* core_count,
                             int* vfp_count,
                             uint32_t* printf_type);
  // Handlers for cases not handled by the assembler.
  // ADD, MOVT, MOVW, SUB, SXTB16, TEQ, UXTB16
  virtual void Delegate(InstructionType type,
                        InstructionCondROp instruction,
                        Condition cond,
                        Register rn,
                        const Operand& operand) VIXL_OVERRIDE;
  // CMN, CMP, MOV, MOVS, MVN, MVNS, SXTB, SXTH, TST, UXTB, UXTH
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeROp instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rn,
                        const Operand& operand) VIXL_OVERRIDE;
  // ADDW, ORN, ORNS, PKHBT, PKHTB, RSC, RSCS, SUBW, SXTAB, SXTAB16, SXTAH,
  // UXTAB, UXTAB16, UXTAH
  virtual void Delegate(InstructionType type,
                        InstructionCondRROp instruction,
                        Condition cond,
                        Register rd,
                        Register rn,
                        const Operand& operand) VIXL_OVERRIDE;
  // ADC, ADCS, ADD, ADDS, AND, ANDS, ASR, ASRS, BIC, BICS, EOR, EORS, LSL,
  // LSLS, LSR, LSRS, ORR, ORRS, ROR, RORS, RSB, RSBS, SBC, SBCS, SUB, SUBS
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRL instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rd,
                        Location* location) VIXL_OVERRIDE;
  bool GenerateSplitInstruction(InstructionCondSizeRROp instruction,
                                Condition cond,
                                Register rd,
                                Register rn,
                                uint32_t imm,
                                uint32_t mask);
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRROp instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rd,
                        Register rn,
                        const Operand& operand) VIXL_OVERRIDE;
  // CBNZ, CBZ
  virtual void Delegate(InstructionType type,
                        InstructionRL instruction,
                        Register rn,
                        Location* location) VIXL_OVERRIDE;
  // VMOV
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSSop instruction,
                        Condition cond,
                        DataType dt,
                        SRegister rd,
                        const SOperand& operand) VIXL_OVERRIDE;
  // VMOV, VMVN
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDDop instruction,
                        Condition cond,
                        DataType dt,
                        DRegister rd,
                        const DOperand& operand) VIXL_OVERRIDE;
  // VMOV, VMVN
  virtual void Delegate(InstructionType type,
                        InstructionCondDtQQop instruction,
                        Condition cond,
                        DataType dt,
                        QRegister rd,
                        const QOperand& operand) VIXL_OVERRIDE;
  // LDR, LDRB, LDRH, LDRSB, LDRSH, STR, STRB, STRH
  virtual void Delegate(InstructionType type,
                        InstructionCondSizeRMop instruction,
                        Condition cond,
                        EncodingSize size,
                        Register rd,
                        const MemOperand& operand) VIXL_OVERRIDE;
  // LDAEXD, LDRD, LDREXD, STLEX, STLEXB, STLEXH, STRD, STREX, STREXB, STREXH
  virtual void Delegate(InstructionType type,
                        InstructionCondRL instruction,
                        Condition cond,
                        Register rt,
                        Location* location) VIXL_OVERRIDE;
  virtual void Delegate(InstructionType type,
                        InstructionCondRRL instruction,
                        Condition cond,
                        Register rt,
                        Register rt2,
                        Location* location) VIXL_OVERRIDE;
  virtual void Delegate(InstructionType type,
                        InstructionCondRRMop instruction,
                        Condition cond,
                        Register rt,
                        Register rt2,
                        const MemOperand& operand) VIXL_OVERRIDE;
  // VLDR, VSTR
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSMop instruction,
                        Condition cond,
                        DataType dt,
                        SRegister rd,
                        const MemOperand& operand) VIXL_OVERRIDE;
  // VLDR, VSTR
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDMop instruction,
                        Condition cond,
                        DataType dt,
                        DRegister rd,
                        const MemOperand& operand) VIXL_OVERRIDE;
  // MSR
  virtual void Delegate(InstructionType type,
                        InstructionCondMsrOp instruction,
                        Condition cond,
                        MaskedSpecialRegister spec_reg,
                        const Operand& operand) VIXL_OVERRIDE;
  virtual void Delegate(InstructionType type,
                        InstructionCondDtDL instruction,
                        Condition cond,
                        DataType dt,
                        DRegister rd,
                        Location* location) VIXL_OVERRIDE;
  virtual void Delegate(InstructionType type,
                        InstructionCondDtSL instruction,
                        Condition cond,
                        DataType dt,
                        SRegister rd,
                        Location* location) VIXL_OVERRIDE;

  // Start of generated code.

  void Adc(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // ADC<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() && rd.Is(rn) &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    adc(cond, rd, rn, operand);
  }
  void Adc(Register rd, Register rn, const Operand& operand) {
    Adc(al, rd, rn, operand);
  }
  void Adc(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Adc(cond, rd, rn, operand);
        break;
      case SetFlags:
        Adcs(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.Is(rd) && operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Adcs(cond, rd, rn, operand);
        } else {
          Adc(cond, rd, rn, operand);
        }
        break;
    }
  }
  void Adc(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Adc(flags, al, rd, rn, operand);
  }

  void Adcs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    adcs(cond, rd, rn, operand);
  }
  void Adcs(Register rd, Register rn, const Operand& operand) {
    Adcs(al, rd, rn, operand);
  }

  void Add(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (cond.Is(al) && rd.Is(rn) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if (immediate == 0) {
        return;
      }
    }
    bool can_use_it =
        // ADD<c>{<q>} <Rd>, <Rn>, #<imm3> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 7) && rn.IsLow() &&
         rd.IsLow()) ||
        // ADD<c>{<q>} {<Rdn>,} <Rdn>, #<imm8> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() <= 255) &&
         rd.IsLow() && rn.Is(rd)) ||
        // ADD{<c>}{<q>} <Rd>, SP, #<imm8> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 1020) &&
         ((operand.GetImmediate() & 0x3) == 0) && rd.IsLow() && rn.IsSP()) ||
        // ADD<c>{<q>} <Rd>, <Rn>, <Rm>
        (operand.IsPlainRegister() && rd.IsLow() && rn.IsLow() &&
         operand.GetBaseRegister().IsLow()) ||
        // ADD<c>{<q>} <Rdn>, <Rm> ; T2
        (operand.IsPlainRegister() && !rd.IsPC() && rn.Is(rd) &&
         !operand.GetBaseRegister().IsSP() &&
         !operand.GetBaseRegister().IsPC()) ||
        // ADD{<c>}{<q>} {<Rdm>,} SP, <Rdm> ; T1
        (operand.IsPlainRegister() && !rd.IsPC() && rn.IsSP() &&
         operand.GetBaseRegister().Is(rd));
    ITScope it_scope(this, &cond, guard, can_use_it);
    add(cond, rd, rn, operand);
  }
  void Add(Register rd, Register rn, const Operand& operand) {
    Add(al, rd, rn, operand);
  }
  void Add(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Add(cond, rd, rn, operand);
        break;
      case SetFlags:
        Adds(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller =
            IsUsingT32() && cond.Is(al) &&
            ((operand.IsPlainRegister() && rd.IsLow() && rn.IsLow() &&
              !rd.Is(rn) && operand.GetBaseRegister().IsLow()) ||
             (operand.IsImmediate() &&
              ((rd.IsLow() && rn.IsLow() && (operand.GetImmediate() < 8)) ||
               (rd.IsLow() && rn.Is(rd) && (operand.GetImmediate() < 256)))));
        if (setflags_is_smaller) {
          Adds(cond, rd, rn, operand);
        } else {
          bool changed_op_is_smaller =
              operand.IsImmediate() && (operand.GetSignedImmediate() < 0) &&
              ((rd.IsLow() && rn.IsLow() &&
                (operand.GetSignedImmediate() >= -7)) ||
               (rd.IsLow() && rn.Is(rd) &&
                (operand.GetSignedImmediate() >= -255)));
          if (changed_op_is_smaller) {
            Subs(cond, rd, rn, -operand.GetSignedImmediate());
          } else {
            Add(cond, rd, rn, operand);
          }
        }
        break;
    }
  }
  void Add(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Add(flags, al, rd, rn, operand);
  }

  void Adds(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    adds(cond, rd, rn, operand);
  }
  void Adds(Register rd, Register rn, const Operand& operand) {
    Adds(al, rd, rn, operand);
  }

  void And(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (rd.Is(rn) && operand.IsPlainRegister() &&
        rd.Is(operand.GetBaseRegister())) {
      return;
    }
    if (cond.Is(al) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if (immediate == 0) {
        mov(rd, 0);
        return;
      }
      if ((immediate == 0xffffffff) && rd.Is(rn)) {
        return;
      }
    }
    bool can_use_it =
        // AND<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    and_(cond, rd, rn, operand);
  }
  void And(Register rd, Register rn, const Operand& operand) {
    And(al, rd, rn, operand);
  }
  void And(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        And(cond, rd, rn, operand);
        break;
      case SetFlags:
        Ands(cond, rd, rn, operand);
        break;
      case DontCare:
        if (operand.IsPlainRegister() && rd.Is(rn) &&
            rd.Is(operand.GetBaseRegister())) {
          return;
        }
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.Is(rd) && operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Ands(cond, rd, rn, operand);
        } else {
          And(cond, rd, rn, operand);
        }
        break;
    }
  }
  void And(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    And(flags, al, rd, rn, operand);
  }

  void Ands(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ands(cond, rd, rn, operand);
  }
  void Ands(Register rd, Register rn, const Operand& operand) {
    Ands(al, rd, rn, operand);
  }

  void Asr(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // ASR<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 32) && rd.IsLow() && rm.IsLow()) ||
        // ASR<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, guard, can_use_it);
    asr(cond, rd, rm, operand);
  }
  void Asr(Register rd, Register rm, const Operand& operand) {
    Asr(al, rd, rm, operand);
  }
  void Asr(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rm,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Asr(cond, rd, rm, operand);
        break;
      case SetFlags:
        Asrs(cond, rd, rm, operand);
        break;
      case DontCare:
        bool setflags_is_smaller =
            IsUsingT32() && cond.Is(al) && rd.IsLow() && rm.IsLow() &&
            ((operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
              (operand.GetImmediate() <= 32)) ||
             (operand.IsPlainRegister() && rd.Is(rm)));
        if (setflags_is_smaller) {
          Asrs(cond, rd, rm, operand);
        } else {
          Asr(cond, rd, rm, operand);
        }
        break;
    }
  }
  void Asr(FlagsUpdate flags,
           Register rd,
           Register rm,
           const Operand& operand) {
    Asr(flags, al, rd, rm, operand);
  }

  void Asrs(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    asrs(cond, rd, rm, operand);
  }
  void Asrs(Register rd, Register rm, const Operand& operand) {
    Asrs(al, rd, rm, operand);
  }

  void B(Condition cond, Label* label, BranchHint hint = kBranchWithoutHint) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    EncodingSize size = Best;
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!label->IsBound()) {
      if (hint == kNear) size = Narrow;
      const ReferenceInfo* info;
      bool can_encode = b_info(cond, size, label, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, label, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    b(cond, size, label);
    RegisterForwardReference(label);
  }
  void B(Label* label, BranchHint hint = kBranchWithoutHint) {
    B(al, label, hint);
  }
  void BPreferNear(Condition cond, Label* label) { B(cond, label, kNear); }
  void BPreferNear(Label* label) { B(al, label, kNear); }

  void Bfc(Condition cond, Register rd, uint32_t lsb, uint32_t width) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    bfc(cond, rd, lsb, width);
  }
  void Bfc(Register rd, uint32_t lsb, uint32_t width) {
    Bfc(al, rd, lsb, width);
  }

  void Bfi(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    bfi(cond, rd, rn, lsb, width);
  }
  void Bfi(Register rd, Register rn, uint32_t lsb, uint32_t width) {
    Bfi(al, rd, rn, lsb, width);
  }

  void Bic(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (cond.Is(al) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if ((immediate == 0) && rd.Is(rn)) {
        return;
      }
      if (immediate == 0xffffffff) {
        mov(rd, 0);
        return;
      }
    }
    bool can_use_it =
        // BIC<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    bic(cond, rd, rn, operand);
  }
  void Bic(Register rd, Register rn, const Operand& operand) {
    Bic(al, rd, rn, operand);
  }
  void Bic(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Bic(cond, rd, rn, operand);
        break;
      case SetFlags:
        Bics(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.Is(rd) && operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Bics(cond, rd, rn, operand);
        } else {
          Bic(cond, rd, rn, operand);
        }
        break;
    }
  }
  void Bic(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Bic(flags, al, rd, rn, operand);
  }

  void Bics(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    bics(cond, rd, rn, operand);
  }
  void Bics(Register rd, Register rn, const Operand& operand) {
    Bics(al, rd, rn, operand);
  }

  void Bkpt(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    bkpt(cond, imm);
  }
  void Bkpt(uint32_t imm) { Bkpt(al, imm); }

  void Bl(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!label->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = bl_info(cond, label, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, label, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    bl(cond, label);
    RegisterForwardReference(label);
  }
  void Bl(Label* label) { Bl(al, label); }

  void Blx(Condition cond, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!label->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = blx_info(cond, label, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, label, &cond);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    ITScope it_scope(this, &cond, guard);
    blx(cond, label);
    RegisterForwardReference(label);
  }
  void Blx(Label* label) { Blx(al, label); }

  void Blx(Condition cond, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // BLX{<c>}{<q>} <Rm> ; T1
        !rm.IsPC();
    ITScope it_scope(this, &cond, guard, can_use_it);
    blx(cond, rm);
  }
  void Blx(Register rm) { Blx(al, rm); }

  void Bx(Condition cond, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // BX{<c>}{<q>} <Rm> ; T1
        !rm.IsPC();
    ITScope it_scope(this, &cond, guard, can_use_it);
    bx(cond, rm);
  }
  void Bx(Register rm) { Bx(al, rm); }

  void Bxj(Condition cond, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    bxj(cond, rm);
  }
  void Bxj(Register rm) { Bxj(al, rm); }

  void Cbnz(Register rn, Label* label) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!label->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = cbnz_info(rn, label, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, label);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    cbnz(rn, label);
    RegisterForwardReference(label);
  }

  void Cbz(Register rn, Label* label) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope::PoolPolicy pool_policy =
        MacroEmissionCheckScope::kBlockPools;
    if (!label->IsBound()) {
      const ReferenceInfo* info;
      bool can_encode = cbz_info(rn, label, &info);
      VIXL_CHECK(can_encode);
      CheckEmitPoolForInstruction(info, label);
      // We have already checked for pool emission.
      pool_policy = MacroEmissionCheckScope::kIgnorePools;
    }
    MacroEmissionCheckScope guard(this, pool_policy);
    cbz(rn, label);
    RegisterForwardReference(label);
  }

  void Clrex(Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    clrex(cond);
  }
  void Clrex() { Clrex(al); }

  void Clz(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    clz(cond, rd, rm);
  }
  void Clz(Register rd, Register rm) { Clz(al, rd, rm); }

  void Cmn(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // CMN{<c>}{<q>} <Rn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    cmn(cond, rn, operand);
  }
  void Cmn(Register rn, const Operand& operand) { Cmn(al, rn, operand); }

  void Cmp(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // CMP{<c>}{<q>} <Rn>, #<imm8> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 255) &&
         rn.IsLow()) ||
        // CMP{<c>}{<q>} <Rn>, <Rm> ; T1 T2
        (operand.IsPlainRegister() && !rn.IsPC() &&
         !operand.GetBaseRegister().IsPC());
    ITScope it_scope(this, &cond, guard, can_use_it);
    cmp(cond, rn, operand);
  }
  void Cmp(Register rn, const Operand& operand) { Cmp(al, rn, operand); }

  void Crc32b(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    crc32b(cond, rd, rn, rm);
  }
  void Crc32b(Register rd, Register rn, Register rm) { Crc32b(al, rd, rn, rm); }

  void Crc32cb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    crc32cb(cond, rd, rn, rm);
  }
  void Crc32cb(Register rd, Register rn, Register rm) {
    Crc32cb(al, rd, rn, rm);
  }

  void Crc32ch(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    crc32ch(cond, rd, rn, rm);
  }
  void Crc32ch(Register rd, Register rn, Register rm) {
    Crc32ch(al, rd, rn, rm);
  }

  void Crc32cw(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    crc32cw(cond, rd, rn, rm);
  }
  void Crc32cw(Register rd, Register rn, Register rm) {
    Crc32cw(al, rd, rn, rm);
  }

  void Crc32h(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    crc32h(cond, rd, rn, rm);
  }
  void Crc32h(Register rd, Register rn, Register rm) { Crc32h(al, rd, rn, rm); }

  void Crc32w(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    crc32w(cond, rd, rn, rm);
  }
  void Crc32w(Register rd, Register rn, Register rm) { Crc32w(al, rd, rn, rm); }

  void Dmb(Condition cond, MemoryBarrier option) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    dmb(cond, option);
  }
  void Dmb(MemoryBarrier option) { Dmb(al, option); }

  void Dsb(Condition cond, MemoryBarrier option) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    dsb(cond, option);
  }
  void Dsb(MemoryBarrier option) { Dsb(al, option); }

  void Eor(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (cond.Is(al) && rd.Is(rn) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if (immediate == 0) {
        return;
      }
      if (immediate == 0xffffffff) {
        mvn(rd, rn);
        return;
      }
    }
    bool can_use_it =
        // EOR<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    eor(cond, rd, rn, operand);
  }
  void Eor(Register rd, Register rn, const Operand& operand) {
    Eor(al, rd, rn, operand);
  }
  void Eor(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Eor(cond, rd, rn, operand);
        break;
      case SetFlags:
        Eors(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.Is(rd) && operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Eors(cond, rd, rn, operand);
        } else {
          Eor(cond, rd, rn, operand);
        }
        break;
    }
  }
  void Eor(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Eor(flags, al, rd, rn, operand);
  }

  void Eors(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    eors(cond, rd, rn, operand);
  }
  void Eors(Register rd, Register rn, const Operand& operand) {
    Eors(al, rd, rn, operand);
  }

  void Fldmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    fldmdbx(cond, rn, write_back, dreglist);
  }
  void Fldmdbx(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fldmdbx(al, rn, write_back, dreglist);
  }

  void Fldmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    fldmiax(cond, rn, write_back, dreglist);
  }
  void Fldmiax(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fldmiax(al, rn, write_back, dreglist);
  }

  void Fstmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    fstmdbx(cond, rn, write_back, dreglist);
  }
  void Fstmdbx(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fstmdbx(al, rn, write_back, dreglist);
  }

  void Fstmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    fstmiax(cond, rn, write_back, dreglist);
  }
  void Fstmiax(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Fstmiax(al, rn, write_back, dreglist);
  }

  void Hlt(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    hlt(cond, imm);
  }
  void Hlt(uint32_t imm) { Hlt(al, imm); }

  void Hvc(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    hvc(cond, imm);
  }
  void Hvc(uint32_t imm) { Hvc(al, imm); }

  void Isb(Condition cond, MemoryBarrier option) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    isb(cond, option);
  }
  void Isb(MemoryBarrier option) { Isb(al, option); }

  void Lda(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    lda(cond, rt, operand);
  }
  void Lda(Register rt, const MemOperand& operand) { Lda(al, rt, operand); }

  void Ldab(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldab(cond, rt, operand);
  }
  void Ldab(Register rt, const MemOperand& operand) { Ldab(al, rt, operand); }

  void Ldaex(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldaex(cond, rt, operand);
  }
  void Ldaex(Register rt, const MemOperand& operand) { Ldaex(al, rt, operand); }

  void Ldaexb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldaexb(cond, rt, operand);
  }
  void Ldaexb(Register rt, const MemOperand& operand) {
    Ldaexb(al, rt, operand);
  }

  void Ldaexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldaexd(cond, rt, rt2, operand);
  }
  void Ldaexd(Register rt, Register rt2, const MemOperand& operand) {
    Ldaexd(al, rt, rt2, operand);
  }

  void Ldaexh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldaexh(cond, rt, operand);
  }
  void Ldaexh(Register rt, const MemOperand& operand) {
    Ldaexh(al, rt, operand);
  }

  void Ldah(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldah(cond, rt, operand);
  }
  void Ldah(Register rt, const MemOperand& operand) { Ldah(al, rt, operand); }

  void Ldm(Condition cond,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldm(cond, rn, write_back, registers);
  }
  void Ldm(Register rn, WriteBack write_back, RegisterList registers) {
    Ldm(al, rn, write_back, registers);
  }

  void Ldmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmda(cond, rn, write_back, registers);
  }
  void Ldmda(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmda(al, rn, write_back, registers);
  }

  void Ldmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmdb(cond, rn, write_back, registers);
  }
  void Ldmdb(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmdb(al, rn, write_back, registers);
  }

  void Ldmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmea(cond, rn, write_back, registers);
  }
  void Ldmea(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmea(al, rn, write_back, registers);
  }

  void Ldmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmed(cond, rn, write_back, registers);
  }
  void Ldmed(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmed(al, rn, write_back, registers);
  }

  void Ldmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmfa(cond, rn, write_back, registers);
  }
  void Ldmfa(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmfa(al, rn, write_back, registers);
  }

  void Ldmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmfd(cond, rn, write_back, registers);
  }
  void Ldmfd(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmfd(al, rn, write_back, registers);
  }

  void Ldmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldmib(cond, rn, write_back, registers);
  }
  void Ldmib(Register rn, WriteBack write_back, RegisterList registers) {
    Ldmib(al, rn, write_back, registers);
  }

  void Ldr(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LDR{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 124, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDR{<c>}{<q>} <Rt>, [SP{, #{+}<imm>}] ; T2
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsSP() &&
         operand.IsOffsetImmediateWithinRange(0, 1020, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDR{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, guard, can_use_it);
    ldr(cond, rt, operand);
  }
  void Ldr(Register rt, const MemOperand& operand) { Ldr(al, rt, operand); }


  void Ldrb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LDRB{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 31) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDRB{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, guard, can_use_it);
    ldrb(cond, rt, operand);
  }
  void Ldrb(Register rt, const MemOperand& operand) { Ldrb(al, rt, operand); }


  void Ldrd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldrd(cond, rt, rt2, operand);
  }
  void Ldrd(Register rt, Register rt2, const MemOperand& operand) {
    Ldrd(al, rt, rt2, operand);
  }


  void Ldrex(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldrex(cond, rt, operand);
  }
  void Ldrex(Register rt, const MemOperand& operand) { Ldrex(al, rt, operand); }

  void Ldrexb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldrexb(cond, rt, operand);
  }
  void Ldrexb(Register rt, const MemOperand& operand) {
    Ldrexb(al, rt, operand);
  }

  void Ldrexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldrexd(cond, rt, rt2, operand);
  }
  void Ldrexd(Register rt, Register rt2, const MemOperand& operand) {
    Ldrexd(al, rt, rt2, operand);
  }

  void Ldrexh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ldrexh(cond, rt, operand);
  }
  void Ldrexh(Register rt, const MemOperand& operand) {
    Ldrexh(al, rt, operand);
  }

  void Ldrh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LDRH{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 62, 2) &&
         (operand.GetAddrMode() == Offset)) ||
        // LDRH{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, guard, can_use_it);
    ldrh(cond, rt, operand);
  }
  void Ldrh(Register rt, const MemOperand& operand) { Ldrh(al, rt, operand); }


  void Ldrsb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LDRSB{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        operand.IsPlainRegister() && rt.IsLow() &&
        operand.GetBaseRegister().IsLow() &&
        operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
        (operand.GetAddrMode() == Offset);
    ITScope it_scope(this, &cond, guard, can_use_it);
    ldrsb(cond, rt, operand);
  }
  void Ldrsb(Register rt, const MemOperand& operand) { Ldrsb(al, rt, operand); }


  void Ldrsh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LDRSH{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        operand.IsPlainRegister() && rt.IsLow() &&
        operand.GetBaseRegister().IsLow() &&
        operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
        (operand.GetAddrMode() == Offset);
    ITScope it_scope(this, &cond, guard, can_use_it);
    ldrsh(cond, rt, operand);
  }
  void Ldrsh(Register rt, const MemOperand& operand) { Ldrsh(al, rt, operand); }


  void Lsl(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LSL<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 31) && rd.IsLow() && rm.IsLow()) ||
        // LSL<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, guard, can_use_it);
    lsl(cond, rd, rm, operand);
  }
  void Lsl(Register rd, Register rm, const Operand& operand) {
    Lsl(al, rd, rm, operand);
  }
  void Lsl(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rm,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Lsl(cond, rd, rm, operand);
        break;
      case SetFlags:
        Lsls(cond, rd, rm, operand);
        break;
      case DontCare:
        bool setflags_is_smaller =
            IsUsingT32() && cond.Is(al) && rd.IsLow() && rm.IsLow() &&
            ((operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
              (operand.GetImmediate() < 32)) ||
             (operand.IsPlainRegister() && rd.Is(rm)));
        if (setflags_is_smaller) {
          Lsls(cond, rd, rm, operand);
        } else {
          Lsl(cond, rd, rm, operand);
        }
        break;
    }
  }
  void Lsl(FlagsUpdate flags,
           Register rd,
           Register rm,
           const Operand& operand) {
    Lsl(flags, al, rd, rm, operand);
  }

  void Lsls(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    lsls(cond, rd, rm, operand);
  }
  void Lsls(Register rd, Register rm, const Operand& operand) {
    Lsls(al, rd, rm, operand);
  }

  void Lsr(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // LSR<c>{<q>} {<Rd>,} <Rm>, #<imm> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
         (operand.GetImmediate() <= 32) && rd.IsLow() && rm.IsLow()) ||
        // LSR<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        (operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, guard, can_use_it);
    lsr(cond, rd, rm, operand);
  }
  void Lsr(Register rd, Register rm, const Operand& operand) {
    Lsr(al, rd, rm, operand);
  }
  void Lsr(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rm,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Lsr(cond, rd, rm, operand);
        break;
      case SetFlags:
        Lsrs(cond, rd, rm, operand);
        break;
      case DontCare:
        bool setflags_is_smaller =
            IsUsingT32() && cond.Is(al) && rd.IsLow() && rm.IsLow() &&
            ((operand.IsImmediate() && (operand.GetImmediate() >= 1) &&
              (operand.GetImmediate() <= 32)) ||
             (operand.IsPlainRegister() && rd.Is(rm)));
        if (setflags_is_smaller) {
          Lsrs(cond, rd, rm, operand);
        } else {
          Lsr(cond, rd, rm, operand);
        }
        break;
    }
  }
  void Lsr(FlagsUpdate flags,
           Register rd,
           Register rm,
           const Operand& operand) {
    Lsr(flags, al, rd, rm, operand);
  }

  void Lsrs(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    lsrs(cond, rd, rm, operand);
  }
  void Lsrs(Register rd, Register rm, const Operand& operand) {
    Lsrs(al, rd, rm, operand);
  }

  void Mla(Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    mla(cond, rd, rn, rm, ra);
  }
  void Mla(Register rd, Register rn, Register rm, Register ra) {
    Mla(al, rd, rn, rm, ra);
  }
  void Mla(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           Register rm,
           Register ra) {
    switch (flags) {
      case LeaveFlags:
        Mla(cond, rd, rn, rm, ra);
        break;
      case SetFlags:
        Mlas(cond, rd, rn, rm, ra);
        break;
      case DontCare:
        Mla(cond, rd, rn, rm, ra);
        break;
    }
  }
  void Mla(
      FlagsUpdate flags, Register rd, Register rn, Register rm, Register ra) {
    Mla(flags, al, rd, rn, rm, ra);
  }

  void Mlas(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    mlas(cond, rd, rn, rm, ra);
  }
  void Mlas(Register rd, Register rn, Register rm, Register ra) {
    Mlas(al, rd, rn, rm, ra);
  }

  void Mls(Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    mls(cond, rd, rn, rm, ra);
  }
  void Mls(Register rd, Register rn, Register rm, Register ra) {
    Mls(al, rd, rn, rm, ra);
  }

  void Mov(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (operand.IsPlainRegister() && rd.Is(operand.GetBaseRegister())) {
      return;
    }
    bool can_use_it =
        // MOV<c>{<q>} <Rd>, #<imm8> ; T1
        (operand.IsImmediate() && rd.IsLow() &&
         (operand.GetImmediate() <= 255)) ||
        // MOV{<c>}{<q>} <Rd>, <Rm> ; T1
        (operand.IsPlainRegister() && !rd.IsPC() &&
         !operand.GetBaseRegister().IsPC()) ||
        // MOV<c>{<q>} <Rd>, <Rm> {, <shift> #<amount>} ; T2
        (operand.IsImmediateShiftedRegister() && rd.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         (operand.GetShift().Is(LSL) || operand.GetShift().Is(LSR) ||
          operand.GetShift().Is(ASR))) ||
        // MOV<c>{<q>} <Rdm>, <Rdm>, LSL <Rs> ; T1
        // MOV<c>{<q>} <Rdm>, <Rdm>, LSR <Rs> ; T1
        // MOV<c>{<q>} <Rdm>, <Rdm>, ASR <Rs> ; T1
        // MOV<c>{<q>} <Rdm>, <Rdm>, ROR <Rs> ; T1
        (operand.IsRegisterShiftedRegister() &&
         rd.Is(operand.GetBaseRegister()) && rd.IsLow() &&
         (operand.GetShift().Is(LSL) || operand.GetShift().Is(LSR) ||
          operand.GetShift().Is(ASR) || operand.GetShift().Is(ROR)) &&
         operand.GetShiftRegister().IsLow());
    ITScope it_scope(this, &cond, guard, can_use_it);
    mov(cond, rd, operand);
  }
  void Mov(Register rd, const Operand& operand) { Mov(al, rd, operand); }
  void Mov(FlagsUpdate flags,
           Condition cond,
           Register rd,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Mov(cond, rd, operand);
        break;
      case SetFlags:
        Movs(cond, rd, operand);
        break;
      case DontCare:
        if (operand.IsPlainRegister() && rd.Is(operand.GetBaseRegister())) {
          return;
        }
        bool setflags_is_smaller =
            IsUsingT32() && cond.Is(al) &&
            ((operand.IsImmediateShiftedRegister() && rd.IsLow() &&
              operand.GetBaseRegister().IsLow() &&
              (operand.GetShiftAmount() >= 1) &&
              (((operand.GetShiftAmount() <= 32) &&
                ((operand.GetShift().IsLSR() || operand.GetShift().IsASR()))) ||
               ((operand.GetShiftAmount() < 32) &&
                operand.GetShift().IsLSL()))) ||
             (operand.IsRegisterShiftedRegister() && rd.IsLow() &&
              operand.GetBaseRegister().Is(rd) &&
              operand.GetShiftRegister().IsLow() &&
              (operand.GetShift().IsLSL() || operand.GetShift().IsLSR() ||
               operand.GetShift().IsASR() || operand.GetShift().IsROR())) ||
             (operand.IsImmediate() && rd.IsLow() &&
              (operand.GetImmediate() < 256)));
        if (setflags_is_smaller) {
          Movs(cond, rd, operand);
        } else {
          Mov(cond, rd, operand);
        }
        break;
    }
  }
  void Mov(FlagsUpdate flags, Register rd, const Operand& operand) {
    Mov(flags, al, rd, operand);
  }

  void Movs(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    movs(cond, rd, operand);
  }
  void Movs(Register rd, const Operand& operand) { Movs(al, rd, operand); }

  void Movt(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    movt(cond, rd, operand);
  }
  void Movt(Register rd, const Operand& operand) { Movt(al, rd, operand); }

  void Mrs(Condition cond, Register rd, SpecialRegister spec_reg) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    mrs(cond, rd, spec_reg);
  }
  void Mrs(Register rd, SpecialRegister spec_reg) { Mrs(al, rd, spec_reg); }

  void Msr(Condition cond,
           MaskedSpecialRegister spec_reg,
           const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    msr(cond, spec_reg, operand);
  }
  void Msr(MaskedSpecialRegister spec_reg, const Operand& operand) {
    Msr(al, spec_reg, operand);
  }

  void Mul(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // MUL<c>{<q>} <Rdm>, <Rn>{, <Rdm>} ; T1
        rd.Is(rm) && rn.IsLow() && rm.IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    mul(cond, rd, rn, rm);
  }
  void Mul(Register rd, Register rn, Register rm) { Mul(al, rd, rn, rm); }
  void Mul(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           Register rm) {
    switch (flags) {
      case LeaveFlags:
        Mul(cond, rd, rn, rm);
        break;
      case SetFlags:
        Muls(cond, rd, rn, rm);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.IsLow() && rm.Is(rd);
        if (setflags_is_smaller) {
          Muls(cond, rd, rn, rm);
        } else {
          Mul(cond, rd, rn, rm);
        }
        break;
    }
  }
  void Mul(FlagsUpdate flags, Register rd, Register rn, Register rm) {
    Mul(flags, al, rd, rn, rm);
  }

  void Muls(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    muls(cond, rd, rn, rm);
  }
  void Muls(Register rd, Register rn, Register rm) { Muls(al, rd, rn, rm); }

  void Mvn(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // MVN<c>{<q>} <Rd>, <Rm> ; T1
        operand.IsPlainRegister() && rd.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    mvn(cond, rd, operand);
  }
  void Mvn(Register rd, const Operand& operand) { Mvn(al, rd, operand); }
  void Mvn(FlagsUpdate flags,
           Condition cond,
           Register rd,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Mvn(cond, rd, operand);
        break;
      case SetFlags:
        Mvns(cond, rd, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Mvns(cond, rd, operand);
        } else {
          Mvn(cond, rd, operand);
        }
        break;
    }
  }
  void Mvn(FlagsUpdate flags, Register rd, const Operand& operand) {
    Mvn(flags, al, rd, operand);
  }

  void Mvns(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    mvns(cond, rd, operand);
  }
  void Mvns(Register rd, const Operand& operand) { Mvns(al, rd, operand); }

  void Nop(Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    nop(cond);
  }
  void Nop() { Nop(al); }

  void Orn(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (cond.Is(al) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if (immediate == 0) {
        mvn(rd, 0);
        return;
      }
      if ((immediate == 0xffffffff) && rd.Is(rn)) {
        return;
      }
    }
    ITScope it_scope(this, &cond, guard);
    orn(cond, rd, rn, operand);
  }
  void Orn(Register rd, Register rn, const Operand& operand) {
    Orn(al, rd, rn, operand);
  }
  void Orn(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Orn(cond, rd, rn, operand);
        break;
      case SetFlags:
        Orns(cond, rd, rn, operand);
        break;
      case DontCare:
        Orn(cond, rd, rn, operand);
        break;
    }
  }
  void Orn(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Orn(flags, al, rd, rn, operand);
  }

  void Orns(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    orns(cond, rd, rn, operand);
  }
  void Orns(Register rd, Register rn, const Operand& operand) {
    Orns(al, rd, rn, operand);
  }

  void Orr(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (rd.Is(rn) && operand.IsPlainRegister() &&
        rd.Is(operand.GetBaseRegister())) {
      return;
    }
    if (cond.Is(al) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if ((immediate == 0) && rd.Is(rn)) {
        return;
      }
      if (immediate == 0xffffffff) {
        mvn(rd, 0);
        return;
      }
    }
    bool can_use_it =
        // ORR<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rd.Is(rn) && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    orr(cond, rd, rn, operand);
  }
  void Orr(Register rd, Register rn, const Operand& operand) {
    Orr(al, rd, rn, operand);
  }
  void Orr(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Orr(cond, rd, rn, operand);
        break;
      case SetFlags:
        Orrs(cond, rd, rn, operand);
        break;
      case DontCare:
        if (operand.IsPlainRegister() && rd.Is(rn) &&
            rd.Is(operand.GetBaseRegister())) {
          return;
        }
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.Is(rd) && operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Orrs(cond, rd, rn, operand);
        } else {
          Orr(cond, rd, rn, operand);
        }
        break;
    }
  }
  void Orr(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Orr(flags, al, rd, rn, operand);
  }

  void Orrs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    orrs(cond, rd, rn, operand);
  }
  void Orrs(Register rd, Register rn, const Operand& operand) {
    Orrs(al, rd, rn, operand);
  }

  void Pkhbt(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pkhbt(cond, rd, rn, operand);
  }
  void Pkhbt(Register rd, Register rn, const Operand& operand) {
    Pkhbt(al, rd, rn, operand);
  }

  void Pkhtb(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pkhtb(cond, rd, rn, operand);
  }
  void Pkhtb(Register rd, Register rn, const Operand& operand) {
    Pkhtb(al, rd, rn, operand);
  }


  void Pld(Condition cond, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pld(cond, operand);
  }
  void Pld(const MemOperand& operand) { Pld(al, operand); }

  void Pldw(Condition cond, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pldw(cond, operand);
  }
  void Pldw(const MemOperand& operand) { Pldw(al, operand); }

  void Pli(Condition cond, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pli(cond, operand);
  }
  void Pli(const MemOperand& operand) { Pli(al, operand); }


  void Pop(Condition cond, RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pop(cond, registers);
  }
  void Pop(RegisterList registers) { Pop(al, registers); }

  void Pop(Condition cond, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    pop(cond, rt);
  }
  void Pop(Register rt) { Pop(al, rt); }

  void Push(Condition cond, RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    push(cond, registers);
  }
  void Push(RegisterList registers) { Push(al, registers); }

  void Push(Condition cond, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    push(cond, rt);
  }
  void Push(Register rt) { Push(al, rt); }

  void Qadd(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qadd(cond, rd, rm, rn);
  }
  void Qadd(Register rd, Register rm, Register rn) { Qadd(al, rd, rm, rn); }

  void Qadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qadd16(cond, rd, rn, rm);
  }
  void Qadd16(Register rd, Register rn, Register rm) { Qadd16(al, rd, rn, rm); }

  void Qadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qadd8(cond, rd, rn, rm);
  }
  void Qadd8(Register rd, Register rn, Register rm) { Qadd8(al, rd, rn, rm); }

  void Qasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qasx(cond, rd, rn, rm);
  }
  void Qasx(Register rd, Register rn, Register rm) { Qasx(al, rd, rn, rm); }

  void Qdadd(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qdadd(cond, rd, rm, rn);
  }
  void Qdadd(Register rd, Register rm, Register rn) { Qdadd(al, rd, rm, rn); }

  void Qdsub(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qdsub(cond, rd, rm, rn);
  }
  void Qdsub(Register rd, Register rm, Register rn) { Qdsub(al, rd, rm, rn); }

  void Qsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qsax(cond, rd, rn, rm);
  }
  void Qsax(Register rd, Register rn, Register rm) { Qsax(al, rd, rn, rm); }

  void Qsub(Condition cond, Register rd, Register rm, Register rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qsub(cond, rd, rm, rn);
  }
  void Qsub(Register rd, Register rm, Register rn) { Qsub(al, rd, rm, rn); }

  void Qsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qsub16(cond, rd, rn, rm);
  }
  void Qsub16(Register rd, Register rn, Register rm) { Qsub16(al, rd, rn, rm); }

  void Qsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    qsub8(cond, rd, rn, rm);
  }
  void Qsub8(Register rd, Register rn, Register rm) { Qsub8(al, rd, rn, rm); }

  void Rbit(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rbit(cond, rd, rm);
  }
  void Rbit(Register rd, Register rm) { Rbit(al, rd, rm); }

  void Rev(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rev(cond, rd, rm);
  }
  void Rev(Register rd, Register rm) { Rev(al, rd, rm); }

  void Rev16(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rev16(cond, rd, rm);
  }
  void Rev16(Register rd, Register rm) { Rev16(al, rd, rm); }

  void Revsh(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    revsh(cond, rd, rm);
  }
  void Revsh(Register rd, Register rm) { Revsh(al, rd, rm); }

  void Ror(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // ROR<c>{<q>} {<Rdm>,} <Rdm>, <Rs> ; T1
        operand.IsPlainRegister() && rd.Is(rm) && rd.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    ror(cond, rd, rm, operand);
  }
  void Ror(Register rd, Register rm, const Operand& operand) {
    Ror(al, rd, rm, operand);
  }
  void Ror(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rm,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Ror(cond, rd, rm, operand);
        break;
      case SetFlags:
        Rors(cond, rd, rm, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rm.IsLow() && operand.IsPlainRegister() &&
                                   rd.Is(rm);
        if (setflags_is_smaller) {
          Rors(cond, rd, rm, operand);
        } else {
          Ror(cond, rd, rm, operand);
        }
        break;
    }
  }
  void Ror(FlagsUpdate flags,
           Register rd,
           Register rm,
           const Operand& operand) {
    Ror(flags, al, rd, rm, operand);
  }

  void Rors(Condition cond, Register rd, Register rm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rors(cond, rd, rm, operand);
  }
  void Rors(Register rd, Register rm, const Operand& operand) {
    Rors(al, rd, rm, operand);
  }

  void Rrx(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rrx(cond, rd, rm);
  }
  void Rrx(Register rd, Register rm) { Rrx(al, rd, rm); }
  void Rrx(FlagsUpdate flags, Condition cond, Register rd, Register rm) {
    switch (flags) {
      case LeaveFlags:
        Rrx(cond, rd, rm);
        break;
      case SetFlags:
        Rrxs(cond, rd, rm);
        break;
      case DontCare:
        Rrx(cond, rd, rm);
        break;
    }
  }
  void Rrx(FlagsUpdate flags, Register rd, Register rm) {
    Rrx(flags, al, rd, rm);
  }

  void Rrxs(Condition cond, Register rd, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rrxs(cond, rd, rm);
  }
  void Rrxs(Register rd, Register rm) { Rrxs(al, rd, rm); }

  void Rsb(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // RSB<c>{<q>} {<Rd>, }<Rn>, #0 ; T1
        operand.IsImmediate() && rd.IsLow() && rn.IsLow() &&
        (operand.GetImmediate() == 0);
    ITScope it_scope(this, &cond, guard, can_use_it);
    rsb(cond, rd, rn, operand);
  }
  void Rsb(Register rd, Register rn, const Operand& operand) {
    Rsb(al, rd, rn, operand);
  }
  void Rsb(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Rsb(cond, rd, rn, operand);
        break;
      case SetFlags:
        Rsbs(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.IsLow() && operand.IsImmediate() &&
                                   (operand.GetImmediate() == 0);
        if (setflags_is_smaller) {
          Rsbs(cond, rd, rn, operand);
        } else {
          Rsb(cond, rd, rn, operand);
        }
        break;
    }
  }
  void Rsb(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Rsb(flags, al, rd, rn, operand);
  }

  void Rsbs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rsbs(cond, rd, rn, operand);
  }
  void Rsbs(Register rd, Register rn, const Operand& operand) {
    Rsbs(al, rd, rn, operand);
  }

  void Rsc(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rsc(cond, rd, rn, operand);
  }
  void Rsc(Register rd, Register rn, const Operand& operand) {
    Rsc(al, rd, rn, operand);
  }
  void Rsc(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Rsc(cond, rd, rn, operand);
        break;
      case SetFlags:
        Rscs(cond, rd, rn, operand);
        break;
      case DontCare:
        Rsc(cond, rd, rn, operand);
        break;
    }
  }
  void Rsc(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Rsc(flags, al, rd, rn, operand);
  }

  void Rscs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    rscs(cond, rd, rn, operand);
  }
  void Rscs(Register rd, Register rn, const Operand& operand) {
    Rscs(al, rd, rn, operand);
  }

  void Sadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sadd16(cond, rd, rn, rm);
  }
  void Sadd16(Register rd, Register rn, Register rm) { Sadd16(al, rd, rn, rm); }

  void Sadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sadd8(cond, rd, rn, rm);
  }
  void Sadd8(Register rd, Register rn, Register rm) { Sadd8(al, rd, rn, rm); }

  void Sasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sasx(cond, rd, rn, rm);
  }
  void Sasx(Register rd, Register rn, Register rm) { Sasx(al, rd, rn, rm); }

  void Sbc(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // SBC<c>{<q>} {<Rdn>,} <Rdn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() && rd.Is(rn) &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    sbc(cond, rd, rn, operand);
  }
  void Sbc(Register rd, Register rn, const Operand& operand) {
    Sbc(al, rd, rn, operand);
  }
  void Sbc(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Sbc(cond, rd, rn, operand);
        break;
      case SetFlags:
        Sbcs(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller = IsUsingT32() && cond.Is(al) && rd.IsLow() &&
                                   rn.Is(rd) && operand.IsPlainRegister() &&
                                   operand.GetBaseRegister().IsLow();
        if (setflags_is_smaller) {
          Sbcs(cond, rd, rn, operand);
        } else {
          Sbc(cond, rd, rn, operand);
        }
        break;
    }
  }
  void Sbc(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Sbc(flags, al, rd, rn, operand);
  }

  void Sbcs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sbcs(cond, rd, rn, operand);
  }
  void Sbcs(Register rd, Register rn, const Operand& operand) {
    Sbcs(al, rd, rn, operand);
  }

  void Sbfx(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sbfx(cond, rd, rn, lsb, width);
  }
  void Sbfx(Register rd, Register rn, uint32_t lsb, uint32_t width) {
    Sbfx(al, rd, rn, lsb, width);
  }

  void Sdiv(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sdiv(cond, rd, rn, rm);
  }
  void Sdiv(Register rd, Register rn, Register rm) { Sdiv(al, rd, rn, rm); }

  void Sel(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sel(cond, rd, rn, rm);
  }
  void Sel(Register rd, Register rn, Register rm) { Sel(al, rd, rn, rm); }

  void Shadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    shadd16(cond, rd, rn, rm);
  }
  void Shadd16(Register rd, Register rn, Register rm) {
    Shadd16(al, rd, rn, rm);
  }

  void Shadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    shadd8(cond, rd, rn, rm);
  }
  void Shadd8(Register rd, Register rn, Register rm) { Shadd8(al, rd, rn, rm); }

  void Shasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    shasx(cond, rd, rn, rm);
  }
  void Shasx(Register rd, Register rn, Register rm) { Shasx(al, rd, rn, rm); }

  void Shsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    shsax(cond, rd, rn, rm);
  }
  void Shsax(Register rd, Register rn, Register rm) { Shsax(al, rd, rn, rm); }

  void Shsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    shsub16(cond, rd, rn, rm);
  }
  void Shsub16(Register rd, Register rn, Register rm) {
    Shsub16(al, rd, rn, rm);
  }

  void Shsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    shsub8(cond, rd, rn, rm);
  }
  void Shsub8(Register rd, Register rn, Register rm) { Shsub8(al, rd, rn, rm); }

  void Smlabb(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlabb(cond, rd, rn, rm, ra);
  }
  void Smlabb(Register rd, Register rn, Register rm, Register ra) {
    Smlabb(al, rd, rn, rm, ra);
  }

  void Smlabt(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlabt(cond, rd, rn, rm, ra);
  }
  void Smlabt(Register rd, Register rn, Register rm, Register ra) {
    Smlabt(al, rd, rn, rm, ra);
  }

  void Smlad(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlad(cond, rd, rn, rm, ra);
  }
  void Smlad(Register rd, Register rn, Register rm, Register ra) {
    Smlad(al, rd, rn, rm, ra);
  }

  void Smladx(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smladx(cond, rd, rn, rm, ra);
  }
  void Smladx(Register rd, Register rn, Register rm, Register ra) {
    Smladx(al, rd, rn, rm, ra);
  }

  void Smlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlal(cond, rdlo, rdhi, rn, rm);
  }
  void Smlal(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlal(al, rdlo, rdhi, rn, rm);
  }

  void Smlalbb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlalbb(cond, rdlo, rdhi, rn, rm);
  }
  void Smlalbb(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlalbb(al, rdlo, rdhi, rn, rm);
  }

  void Smlalbt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlalbt(cond, rdlo, rdhi, rn, rm);
  }
  void Smlalbt(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlalbt(al, rdlo, rdhi, rn, rm);
  }

  void Smlald(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlald(cond, rdlo, rdhi, rn, rm);
  }
  void Smlald(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlald(al, rdlo, rdhi, rn, rm);
  }

  void Smlaldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlaldx(cond, rdlo, rdhi, rn, rm);
  }
  void Smlaldx(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlaldx(al, rdlo, rdhi, rn, rm);
  }

  void Smlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlals(cond, rdlo, rdhi, rn, rm);
  }
  void Smlals(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlals(al, rdlo, rdhi, rn, rm);
  }

  void Smlaltb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlaltb(cond, rdlo, rdhi, rn, rm);
  }
  void Smlaltb(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlaltb(al, rdlo, rdhi, rn, rm);
  }

  void Smlaltt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlaltt(cond, rdlo, rdhi, rn, rm);
  }
  void Smlaltt(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlaltt(al, rdlo, rdhi, rn, rm);
  }

  void Smlatb(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlatb(cond, rd, rn, rm, ra);
  }
  void Smlatb(Register rd, Register rn, Register rm, Register ra) {
    Smlatb(al, rd, rn, rm, ra);
  }

  void Smlatt(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlatt(cond, rd, rn, rm, ra);
  }
  void Smlatt(Register rd, Register rn, Register rm, Register ra) {
    Smlatt(al, rd, rn, rm, ra);
  }

  void Smlawb(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlawb(cond, rd, rn, rm, ra);
  }
  void Smlawb(Register rd, Register rn, Register rm, Register ra) {
    Smlawb(al, rd, rn, rm, ra);
  }

  void Smlawt(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlawt(cond, rd, rn, rm, ra);
  }
  void Smlawt(Register rd, Register rn, Register rm, Register ra) {
    Smlawt(al, rd, rn, rm, ra);
  }

  void Smlsd(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlsd(cond, rd, rn, rm, ra);
  }
  void Smlsd(Register rd, Register rn, Register rm, Register ra) {
    Smlsd(al, rd, rn, rm, ra);
  }

  void Smlsdx(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlsdx(cond, rd, rn, rm, ra);
  }
  void Smlsdx(Register rd, Register rn, Register rm, Register ra) {
    Smlsdx(al, rd, rn, rm, ra);
  }

  void Smlsld(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlsld(cond, rdlo, rdhi, rn, rm);
  }
  void Smlsld(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlsld(al, rdlo, rdhi, rn, rm);
  }

  void Smlsldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smlsldx(cond, rdlo, rdhi, rn, rm);
  }
  void Smlsldx(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smlsldx(al, rdlo, rdhi, rn, rm);
  }

  void Smmla(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smmla(cond, rd, rn, rm, ra);
  }
  void Smmla(Register rd, Register rn, Register rm, Register ra) {
    Smmla(al, rd, rn, rm, ra);
  }

  void Smmlar(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smmlar(cond, rd, rn, rm, ra);
  }
  void Smmlar(Register rd, Register rn, Register rm, Register ra) {
    Smmlar(al, rd, rn, rm, ra);
  }

  void Smmls(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smmls(cond, rd, rn, rm, ra);
  }
  void Smmls(Register rd, Register rn, Register rm, Register ra) {
    Smmls(al, rd, rn, rm, ra);
  }

  void Smmlsr(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smmlsr(cond, rd, rn, rm, ra);
  }
  void Smmlsr(Register rd, Register rn, Register rm, Register ra) {
    Smmlsr(al, rd, rn, rm, ra);
  }

  void Smmul(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smmul(cond, rd, rn, rm);
  }
  void Smmul(Register rd, Register rn, Register rm) { Smmul(al, rd, rn, rm); }

  void Smmulr(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smmulr(cond, rd, rn, rm);
  }
  void Smmulr(Register rd, Register rn, Register rm) { Smmulr(al, rd, rn, rm); }

  void Smuad(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smuad(cond, rd, rn, rm);
  }
  void Smuad(Register rd, Register rn, Register rm) { Smuad(al, rd, rn, rm); }

  void Smuadx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smuadx(cond, rd, rn, rm);
  }
  void Smuadx(Register rd, Register rn, Register rm) { Smuadx(al, rd, rn, rm); }

  void Smulbb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smulbb(cond, rd, rn, rm);
  }
  void Smulbb(Register rd, Register rn, Register rm) { Smulbb(al, rd, rn, rm); }

  void Smulbt(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smulbt(cond, rd, rn, rm);
  }
  void Smulbt(Register rd, Register rn, Register rm) { Smulbt(al, rd, rn, rm); }

  void Smull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smull(cond, rdlo, rdhi, rn, rm);
  }
  void Smull(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smull(al, rdlo, rdhi, rn, rm);
  }
  void Smull(FlagsUpdate flags,
             Condition cond,
             Register rdlo,
             Register rdhi,
             Register rn,
             Register rm) {
    switch (flags) {
      case LeaveFlags:
        Smull(cond, rdlo, rdhi, rn, rm);
        break;
      case SetFlags:
        Smulls(cond, rdlo, rdhi, rn, rm);
        break;
      case DontCare:
        Smull(cond, rdlo, rdhi, rn, rm);
        break;
    }
  }
  void Smull(FlagsUpdate flags,
             Register rdlo,
             Register rdhi,
             Register rn,
             Register rm) {
    Smull(flags, al, rdlo, rdhi, rn, rm);
  }

  void Smulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smulls(cond, rdlo, rdhi, rn, rm);
  }
  void Smulls(Register rdlo, Register rdhi, Register rn, Register rm) {
    Smulls(al, rdlo, rdhi, rn, rm);
  }

  void Smultb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smultb(cond, rd, rn, rm);
  }
  void Smultb(Register rd, Register rn, Register rm) { Smultb(al, rd, rn, rm); }

  void Smultt(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smultt(cond, rd, rn, rm);
  }
  void Smultt(Register rd, Register rn, Register rm) { Smultt(al, rd, rn, rm); }

  void Smulwb(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smulwb(cond, rd, rn, rm);
  }
  void Smulwb(Register rd, Register rn, Register rm) { Smulwb(al, rd, rn, rm); }

  void Smulwt(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smulwt(cond, rd, rn, rm);
  }
  void Smulwt(Register rd, Register rn, Register rm) { Smulwt(al, rd, rn, rm); }

  void Smusd(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smusd(cond, rd, rn, rm);
  }
  void Smusd(Register rd, Register rn, Register rm) { Smusd(al, rd, rn, rm); }

  void Smusdx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    smusdx(cond, rd, rn, rm);
  }
  void Smusdx(Register rd, Register rn, Register rm) { Smusdx(al, rd, rn, rm); }

  void Ssat(Condition cond, Register rd, uint32_t imm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ssat(cond, rd, imm, operand);
  }
  void Ssat(Register rd, uint32_t imm, const Operand& operand) {
    Ssat(al, rd, imm, operand);
  }

  void Ssat16(Condition cond, Register rd, uint32_t imm, Register rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ssat16(cond, rd, imm, rn);
  }
  void Ssat16(Register rd, uint32_t imm, Register rn) {
    Ssat16(al, rd, imm, rn);
  }

  void Ssax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ssax(cond, rd, rn, rm);
  }
  void Ssax(Register rd, Register rn, Register rm) { Ssax(al, rd, rn, rm); }

  void Ssub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ssub16(cond, rd, rn, rm);
  }
  void Ssub16(Register rd, Register rn, Register rm) { Ssub16(al, rd, rn, rm); }

  void Ssub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ssub8(cond, rd, rn, rm);
  }
  void Ssub8(Register rd, Register rn, Register rm) { Ssub8(al, rd, rn, rm); }

  void Stl(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stl(cond, rt, operand);
  }
  void Stl(Register rt, const MemOperand& operand) { Stl(al, rt, operand); }

  void Stlb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stlb(cond, rt, operand);
  }
  void Stlb(Register rt, const MemOperand& operand) { Stlb(al, rt, operand); }

  void Stlex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stlex(cond, rd, rt, operand);
  }
  void Stlex(Register rd, Register rt, const MemOperand& operand) {
    Stlex(al, rd, rt, operand);
  }

  void Stlexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stlexb(cond, rd, rt, operand);
  }
  void Stlexb(Register rd, Register rt, const MemOperand& operand) {
    Stlexb(al, rd, rt, operand);
  }

  void Stlexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stlexd(cond, rd, rt, rt2, operand);
  }
  void Stlexd(Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    Stlexd(al, rd, rt, rt2, operand);
  }

  void Stlexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stlexh(cond, rd, rt, operand);
  }
  void Stlexh(Register rd, Register rt, const MemOperand& operand) {
    Stlexh(al, rd, rt, operand);
  }

  void Stlh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stlh(cond, rt, operand);
  }
  void Stlh(Register rt, const MemOperand& operand) { Stlh(al, rt, operand); }

  void Stm(Condition cond,
           Register rn,
           WriteBack write_back,
           RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stm(cond, rn, write_back, registers);
  }
  void Stm(Register rn, WriteBack write_back, RegisterList registers) {
    Stm(al, rn, write_back, registers);
  }

  void Stmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmda(cond, rn, write_back, registers);
  }
  void Stmda(Register rn, WriteBack write_back, RegisterList registers) {
    Stmda(al, rn, write_back, registers);
  }

  void Stmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmdb(cond, rn, write_back, registers);
  }
  void Stmdb(Register rn, WriteBack write_back, RegisterList registers) {
    Stmdb(al, rn, write_back, registers);
  }

  void Stmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmea(cond, rn, write_back, registers);
  }
  void Stmea(Register rn, WriteBack write_back, RegisterList registers) {
    Stmea(al, rn, write_back, registers);
  }

  void Stmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmed(cond, rn, write_back, registers);
  }
  void Stmed(Register rn, WriteBack write_back, RegisterList registers) {
    Stmed(al, rn, write_back, registers);
  }

  void Stmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmfa(cond, rn, write_back, registers);
  }
  void Stmfa(Register rn, WriteBack write_back, RegisterList registers) {
    Stmfa(al, rn, write_back, registers);
  }

  void Stmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmfd(cond, rn, write_back, registers);
  }
  void Stmfd(Register rn, WriteBack write_back, RegisterList registers) {
    Stmfd(al, rn, write_back, registers);
  }

  void Stmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(registers));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    stmib(cond, rn, write_back, registers);
  }
  void Stmib(Register rn, WriteBack write_back, RegisterList registers) {
    Stmib(al, rn, write_back, registers);
  }

  void Str(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // STR{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 124, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // STR{<c>}{<q>} <Rt>, [SP{, #{+}<imm>}] ; T2
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsSP() &&
         operand.IsOffsetImmediateWithinRange(0, 1020, 4) &&
         (operand.GetAddrMode() == Offset)) ||
        // STR{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, guard, can_use_it);
    str(cond, rt, operand);
  }
  void Str(Register rt, const MemOperand& operand) { Str(al, rt, operand); }

  void Strb(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // STRB{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 31) &&
         (operand.GetAddrMode() == Offset)) ||
        // STRB{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, guard, can_use_it);
    strb(cond, rt, operand);
  }
  void Strb(Register rt, const MemOperand& operand) { Strb(al, rt, operand); }

  void Strd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    strd(cond, rt, rt2, operand);
  }
  void Strd(Register rt, Register rt2, const MemOperand& operand) {
    Strd(al, rt, rt2, operand);
  }

  void Strex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    strex(cond, rd, rt, operand);
  }
  void Strex(Register rd, Register rt, const MemOperand& operand) {
    Strex(al, rd, rt, operand);
  }

  void Strexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    strexb(cond, rd, rt, operand);
  }
  void Strexb(Register rd, Register rt, const MemOperand& operand) {
    Strexb(al, rd, rt, operand);
  }

  void Strexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    strexd(cond, rd, rt, rt2, operand);
  }
  void Strexd(Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand) {
    Strexd(al, rd, rt, rt2, operand);
  }

  void Strexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    strexh(cond, rd, rt, operand);
  }
  void Strexh(Register rd, Register rt, const MemOperand& operand) {
    Strexh(al, rd, rt, operand);
  }

  void Strh(Condition cond, Register rt, const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // STRH{<c>}{<q>} <Rt>, [<Rn> {, #{+}<imm>}] ; T1
        (operand.IsImmediate() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.IsOffsetImmediateWithinRange(0, 62, 2) &&
         (operand.GetAddrMode() == Offset)) ||
        // STRH{<c>}{<q>} <Rt>, [<Rn>, {+}<Rm>] ; T1
        (operand.IsPlainRegister() && rt.IsLow() &&
         operand.GetBaseRegister().IsLow() &&
         operand.GetOffsetRegister().IsLow() && operand.GetSign().IsPlus() &&
         (operand.GetAddrMode() == Offset));
    ITScope it_scope(this, &cond, guard, can_use_it);
    strh(cond, rt, operand);
  }
  void Strh(Register rt, const MemOperand& operand) { Strh(al, rt, operand); }

  void Sub(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    if (cond.Is(al) && rd.Is(rn) && operand.IsImmediate()) {
      uint32_t immediate = operand.GetImmediate();
      if (immediate == 0) {
        return;
      }
    }
    bool can_use_it =
        // SUB<c>{<q>} <Rd>, <Rn>, #<imm3> ; T1
        (operand.IsImmediate() && (operand.GetImmediate() <= 7) && rn.IsLow() &&
         rd.IsLow()) ||
        // SUB<c>{<q>} {<Rdn>,} <Rdn>, #<imm8> ; T2
        (operand.IsImmediate() && (operand.GetImmediate() <= 255) &&
         rd.IsLow() && rn.Is(rd)) ||
        // SUB<c>{<q>} <Rd>, <Rn>, <Rm>
        (operand.IsPlainRegister() && rd.IsLow() && rn.IsLow() &&
         operand.GetBaseRegister().IsLow());
    ITScope it_scope(this, &cond, guard, can_use_it);
    sub(cond, rd, rn, operand);
  }
  void Sub(Register rd, Register rn, const Operand& operand) {
    Sub(al, rd, rn, operand);
  }
  void Sub(FlagsUpdate flags,
           Condition cond,
           Register rd,
           Register rn,
           const Operand& operand) {
    switch (flags) {
      case LeaveFlags:
        Sub(cond, rd, rn, operand);
        break;
      case SetFlags:
        Subs(cond, rd, rn, operand);
        break;
      case DontCare:
        bool setflags_is_smaller =
            IsUsingT32() && cond.Is(al) &&
            ((operand.IsPlainRegister() && rd.IsLow() && rn.IsLow() &&
              operand.GetBaseRegister().IsLow()) ||
             (operand.IsImmediate() &&
              ((rd.IsLow() && rn.IsLow() && (operand.GetImmediate() < 8)) ||
               (rd.IsLow() && rn.Is(rd) && (operand.GetImmediate() < 256)))));
        if (setflags_is_smaller) {
          Subs(cond, rd, rn, operand);
        } else {
          bool changed_op_is_smaller =
              operand.IsImmediate() && (operand.GetSignedImmediate() < 0) &&
              ((rd.IsLow() && rn.IsLow() &&
                (operand.GetSignedImmediate() >= -7)) ||
               (rd.IsLow() && rn.Is(rd) &&
                (operand.GetSignedImmediate() >= -255)));
          if (changed_op_is_smaller) {
            Adds(cond, rd, rn, -operand.GetSignedImmediate());
          } else {
            Sub(cond, rd, rn, operand);
          }
        }
        break;
    }
  }
  void Sub(FlagsUpdate flags,
           Register rd,
           Register rn,
           const Operand& operand) {
    Sub(flags, al, rd, rn, operand);
  }

  void Subs(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    subs(cond, rd, rn, operand);
  }
  void Subs(Register rd, Register rn, const Operand& operand) {
    Subs(al, rd, rn, operand);
  }

  void Svc(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    svc(cond, imm);
  }
  void Svc(uint32_t imm) { Svc(al, imm); }

  void Sxtab(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sxtab(cond, rd, rn, operand);
  }
  void Sxtab(Register rd, Register rn, const Operand& operand) {
    Sxtab(al, rd, rn, operand);
  }

  void Sxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sxtab16(cond, rd, rn, operand);
  }
  void Sxtab16(Register rd, Register rn, const Operand& operand) {
    Sxtab16(al, rd, rn, operand);
  }

  void Sxtah(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sxtah(cond, rd, rn, operand);
  }
  void Sxtah(Register rd, Register rn, const Operand& operand) {
    Sxtah(al, rd, rn, operand);
  }

  void Sxtb(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sxtb(cond, rd, operand);
  }
  void Sxtb(Register rd, const Operand& operand) { Sxtb(al, rd, operand); }

  void Sxtb16(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sxtb16(cond, rd, operand);
  }
  void Sxtb16(Register rd, const Operand& operand) { Sxtb16(al, rd, operand); }

  void Sxth(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    sxth(cond, rd, operand);
  }
  void Sxth(Register rd, const Operand& operand) { Sxth(al, rd, operand); }

  void Teq(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    teq(cond, rn, operand);
  }
  void Teq(Register rn, const Operand& operand) { Teq(al, rn, operand); }

  void Tst(Condition cond, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    bool can_use_it =
        // TST{<c>}{<q>} <Rn>, <Rm> ; T1
        operand.IsPlainRegister() && rn.IsLow() &&
        operand.GetBaseRegister().IsLow();
    ITScope it_scope(this, &cond, guard, can_use_it);
    tst(cond, rn, operand);
  }
  void Tst(Register rn, const Operand& operand) { Tst(al, rn, operand); }

  void Uadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uadd16(cond, rd, rn, rm);
  }
  void Uadd16(Register rd, Register rn, Register rm) { Uadd16(al, rd, rn, rm); }

  void Uadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uadd8(cond, rd, rn, rm);
  }
  void Uadd8(Register rd, Register rn, Register rm) { Uadd8(al, rd, rn, rm); }

  void Uasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uasx(cond, rd, rn, rm);
  }
  void Uasx(Register rd, Register rn, Register rm) { Uasx(al, rd, rn, rm); }

  void Ubfx(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    ubfx(cond, rd, rn, lsb, width);
  }
  void Ubfx(Register rd, Register rn, uint32_t lsb, uint32_t width) {
    Ubfx(al, rd, rn, lsb, width);
  }

  void Udf(Condition cond, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    udf(cond, imm);
  }
  void Udf(uint32_t imm) { Udf(al, imm); }

  void Udiv(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    udiv(cond, rd, rn, rm);
  }
  void Udiv(Register rd, Register rn, Register rm) { Udiv(al, rd, rn, rm); }

  void Uhadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uhadd16(cond, rd, rn, rm);
  }
  void Uhadd16(Register rd, Register rn, Register rm) {
    Uhadd16(al, rd, rn, rm);
  }

  void Uhadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uhadd8(cond, rd, rn, rm);
  }
  void Uhadd8(Register rd, Register rn, Register rm) { Uhadd8(al, rd, rn, rm); }

  void Uhasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uhasx(cond, rd, rn, rm);
  }
  void Uhasx(Register rd, Register rn, Register rm) { Uhasx(al, rd, rn, rm); }

  void Uhsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uhsax(cond, rd, rn, rm);
  }
  void Uhsax(Register rd, Register rn, Register rm) { Uhsax(al, rd, rn, rm); }

  void Uhsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uhsub16(cond, rd, rn, rm);
  }
  void Uhsub16(Register rd, Register rn, Register rm) {
    Uhsub16(al, rd, rn, rm);
  }

  void Uhsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uhsub8(cond, rd, rn, rm);
  }
  void Uhsub8(Register rd, Register rn, Register rm) { Uhsub8(al, rd, rn, rm); }

  void Umaal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    umaal(cond, rdlo, rdhi, rn, rm);
  }
  void Umaal(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umaal(al, rdlo, rdhi, rn, rm);
  }

  void Umlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    umlal(cond, rdlo, rdhi, rn, rm);
  }
  void Umlal(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umlal(al, rdlo, rdhi, rn, rm);
  }
  void Umlal(FlagsUpdate flags,
             Condition cond,
             Register rdlo,
             Register rdhi,
             Register rn,
             Register rm) {
    switch (flags) {
      case LeaveFlags:
        Umlal(cond, rdlo, rdhi, rn, rm);
        break;
      case SetFlags:
        Umlals(cond, rdlo, rdhi, rn, rm);
        break;
      case DontCare:
        Umlal(cond, rdlo, rdhi, rn, rm);
        break;
    }
  }
  void Umlal(FlagsUpdate flags,
             Register rdlo,
             Register rdhi,
             Register rn,
             Register rm) {
    Umlal(flags, al, rdlo, rdhi, rn, rm);
  }

  void Umlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    umlals(cond, rdlo, rdhi, rn, rm);
  }
  void Umlals(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umlals(al, rdlo, rdhi, rn, rm);
  }

  void Umull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    umull(cond, rdlo, rdhi, rn, rm);
  }
  void Umull(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umull(al, rdlo, rdhi, rn, rm);
  }
  void Umull(FlagsUpdate flags,
             Condition cond,
             Register rdlo,
             Register rdhi,
             Register rn,
             Register rm) {
    switch (flags) {
      case LeaveFlags:
        Umull(cond, rdlo, rdhi, rn, rm);
        break;
      case SetFlags:
        Umulls(cond, rdlo, rdhi, rn, rm);
        break;
      case DontCare:
        Umull(cond, rdlo, rdhi, rn, rm);
        break;
    }
  }
  void Umull(FlagsUpdate flags,
             Register rdlo,
             Register rdhi,
             Register rn,
             Register rm) {
    Umull(flags, al, rdlo, rdhi, rn, rm);
  }

  void Umulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdlo));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rdhi));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    umulls(cond, rdlo, rdhi, rn, rm);
  }
  void Umulls(Register rdlo, Register rdhi, Register rn, Register rm) {
    Umulls(al, rdlo, rdhi, rn, rm);
  }

  void Uqadd16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uqadd16(cond, rd, rn, rm);
  }
  void Uqadd16(Register rd, Register rn, Register rm) {
    Uqadd16(al, rd, rn, rm);
  }

  void Uqadd8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uqadd8(cond, rd, rn, rm);
  }
  void Uqadd8(Register rd, Register rn, Register rm) { Uqadd8(al, rd, rn, rm); }

  void Uqasx(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uqasx(cond, rd, rn, rm);
  }
  void Uqasx(Register rd, Register rn, Register rm) { Uqasx(al, rd, rn, rm); }

  void Uqsax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uqsax(cond, rd, rn, rm);
  }
  void Uqsax(Register rd, Register rn, Register rm) { Uqsax(al, rd, rn, rm); }

  void Uqsub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uqsub16(cond, rd, rn, rm);
  }
  void Uqsub16(Register rd, Register rn, Register rm) {
    Uqsub16(al, rd, rn, rm);
  }

  void Uqsub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uqsub8(cond, rd, rn, rm);
  }
  void Uqsub8(Register rd, Register rn, Register rm) { Uqsub8(al, rd, rn, rm); }

  void Usad8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usad8(cond, rd, rn, rm);
  }
  void Usad8(Register rd, Register rn, Register rm) { Usad8(al, rd, rn, rm); }

  void Usada8(
      Condition cond, Register rd, Register rn, Register rm, Register ra) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(ra));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usada8(cond, rd, rn, rm, ra);
  }
  void Usada8(Register rd, Register rn, Register rm, Register ra) {
    Usada8(al, rd, rn, rm, ra);
  }

  void Usat(Condition cond, Register rd, uint32_t imm, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usat(cond, rd, imm, operand);
  }
  void Usat(Register rd, uint32_t imm, const Operand& operand) {
    Usat(al, rd, imm, operand);
  }

  void Usat16(Condition cond, Register rd, uint32_t imm, Register rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usat16(cond, rd, imm, rn);
  }
  void Usat16(Register rd, uint32_t imm, Register rn) {
    Usat16(al, rd, imm, rn);
  }

  void Usax(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usax(cond, rd, rn, rm);
  }
  void Usax(Register rd, Register rn, Register rm) { Usax(al, rd, rn, rm); }

  void Usub16(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usub16(cond, rd, rn, rm);
  }
  void Usub16(Register rd, Register rn, Register rm) { Usub16(al, rd, rn, rm); }

  void Usub8(Condition cond, Register rd, Register rn, Register rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    usub8(cond, rd, rn, rm);
  }
  void Usub8(Register rd, Register rn, Register rm) { Usub8(al, rd, rn, rm); }

  void Uxtab(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uxtab(cond, rd, rn, operand);
  }
  void Uxtab(Register rd, Register rn, const Operand& operand) {
    Uxtab(al, rd, rn, operand);
  }

  void Uxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uxtab16(cond, rd, rn, operand);
  }
  void Uxtab16(Register rd, Register rn, const Operand& operand) {
    Uxtab16(al, rd, rn, operand);
  }

  void Uxtah(Condition cond, Register rd, Register rn, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uxtah(cond, rd, rn, operand);
  }
  void Uxtah(Register rd, Register rn, const Operand& operand) {
    Uxtah(al, rd, rn, operand);
  }

  void Uxtb(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uxtb(cond, rd, operand);
  }
  void Uxtb(Register rd, const Operand& operand) { Uxtb(al, rd, operand); }

  void Uxtb16(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uxtb16(cond, rd, operand);
  }
  void Uxtb16(Register rd, const Operand& operand) { Uxtb16(al, rd, operand); }

  void Uxth(Condition cond, Register rd, const Operand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    uxth(cond, rd, operand);
  }
  void Uxth(Register rd, const Operand& operand) { Uxth(al, rd, operand); }

  void Vaba(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaba(cond, dt, rd, rn, rm);
  }
  void Vaba(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vaba(al, dt, rd, rn, rm);
  }

  void Vaba(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaba(cond, dt, rd, rn, rm);
  }
  void Vaba(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vaba(al, dt, rd, rn, rm);
  }

  void Vabal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabal(cond, dt, rd, rn, rm);
  }
  void Vabal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vabal(al, dt, rd, rn, rm);
  }

  void Vabd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabd(cond, dt, rd, rn, rm);
  }
  void Vabd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vabd(al, dt, rd, rn, rm);
  }

  void Vabd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabd(cond, dt, rd, rn, rm);
  }
  void Vabd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vabd(al, dt, rd, rn, rm);
  }

  void Vabdl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabdl(cond, dt, rd, rn, rm);
  }
  void Vabdl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vabdl(al, dt, rd, rn, rm);
  }

  void Vabs(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabs(cond, dt, rd, rm);
  }
  void Vabs(DataType dt, DRegister rd, DRegister rm) { Vabs(al, dt, rd, rm); }

  void Vabs(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabs(cond, dt, rd, rm);
  }
  void Vabs(DataType dt, QRegister rd, QRegister rm) { Vabs(al, dt, rd, rm); }

  void Vabs(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vabs(cond, dt, rd, rm);
  }
  void Vabs(DataType dt, SRegister rd, SRegister rm) { Vabs(al, dt, rd, rm); }

  void Vacge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vacge(cond, dt, rd, rn, rm);
  }
  void Vacge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vacge(al, dt, rd, rn, rm);
  }

  void Vacge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vacge(cond, dt, rd, rn, rm);
  }
  void Vacge(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vacge(al, dt, rd, rn, rm);
  }

  void Vacgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vacgt(cond, dt, rd, rn, rm);
  }
  void Vacgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vacgt(al, dt, rd, rn, rm);
  }

  void Vacgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vacgt(cond, dt, rd, rn, rm);
  }
  void Vacgt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vacgt(al, dt, rd, rn, rm);
  }

  void Vacle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vacle(cond, dt, rd, rn, rm);
  }
  void Vacle(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vacle(al, dt, rd, rn, rm);
  }

  void Vacle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vacle(cond, dt, rd, rn, rm);
  }
  void Vacle(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vacle(al, dt, rd, rn, rm);
  }

  void Vaclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaclt(cond, dt, rd, rn, rm);
  }
  void Vaclt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vaclt(al, dt, rd, rn, rm);
  }

  void Vaclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaclt(cond, dt, rd, rn, rm);
  }
  void Vaclt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vaclt(al, dt, rd, rn, rm);
  }

  void Vadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vadd(cond, dt, rd, rn, rm);
  }
  void Vadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vadd(al, dt, rd, rn, rm);
  }

  void Vadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vadd(cond, dt, rd, rn, rm);
  }
  void Vadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vadd(al, dt, rd, rn, rm);
  }

  void Vadd(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vadd(cond, dt, rd, rn, rm);
  }
  void Vadd(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vadd(al, dt, rd, rn, rm);
  }

  void Vaddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaddhn(cond, dt, rd, rn, rm);
  }
  void Vaddhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vaddhn(al, dt, rd, rn, rm);
  }

  void Vaddl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaddl(cond, dt, rd, rn, rm);
  }
  void Vaddl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vaddl(al, dt, rd, rn, rm);
  }

  void Vaddw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vaddw(cond, dt, rd, rn, rm);
  }
  void Vaddw(DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    Vaddw(al, dt, rd, rn, rm);
  }

  void Vand(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vand(cond, dt, rd, rn, operand);
  }
  void Vand(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vand(al, dt, rd, rn, operand);
  }

  void Vand(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vand(cond, dt, rd, rn, operand);
  }
  void Vand(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vand(al, dt, rd, rn, operand);
  }

  void Vbic(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbic(cond, dt, rd, rn, operand);
  }
  void Vbic(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vbic(al, dt, rd, rn, operand);
  }

  void Vbic(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbic(cond, dt, rd, rn, operand);
  }
  void Vbic(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vbic(al, dt, rd, rn, operand);
  }

  void Vbif(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbif(cond, dt, rd, rn, rm);
  }
  void Vbif(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vbif(al, dt, rd, rn, rm);
  }
  void Vbif(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Vbif(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbif(DRegister rd, DRegister rn, DRegister rm) {
    Vbif(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbif(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbif(cond, dt, rd, rn, rm);
  }
  void Vbif(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vbif(al, dt, rd, rn, rm);
  }
  void Vbif(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Vbif(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbif(QRegister rd, QRegister rn, QRegister rm) {
    Vbif(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbit(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbit(cond, dt, rd, rn, rm);
  }
  void Vbit(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vbit(al, dt, rd, rn, rm);
  }
  void Vbit(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Vbit(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbit(DRegister rd, DRegister rn, DRegister rm) {
    Vbit(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbit(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbit(cond, dt, rd, rn, rm);
  }
  void Vbit(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vbit(al, dt, rd, rn, rm);
  }
  void Vbit(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Vbit(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbit(QRegister rd, QRegister rn, QRegister rm) {
    Vbit(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbsl(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbsl(cond, dt, rd, rn, rm);
  }
  void Vbsl(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vbsl(al, dt, rd, rn, rm);
  }
  void Vbsl(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Vbsl(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbsl(DRegister rd, DRegister rn, DRegister rm) {
    Vbsl(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vbsl(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vbsl(cond, dt, rd, rn, rm);
  }
  void Vbsl(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vbsl(al, dt, rd, rn, rm);
  }
  void Vbsl(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Vbsl(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Vbsl(QRegister rd, QRegister rn, QRegister rm) {
    Vbsl(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vceq(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vceq(cond, dt, rd, rm, operand);
  }
  void Vceq(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vceq(al, dt, rd, rm, operand);
  }

  void Vceq(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vceq(cond, dt, rd, rm, operand);
  }
  void Vceq(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vceq(al, dt, rd, rm, operand);
  }

  void Vceq(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vceq(cond, dt, rd, rn, rm);
  }
  void Vceq(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vceq(al, dt, rd, rn, rm);
  }

  void Vceq(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vceq(cond, dt, rd, rn, rm);
  }
  void Vceq(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vceq(al, dt, rd, rn, rm);
  }

  void Vcge(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcge(cond, dt, rd, rm, operand);
  }
  void Vcge(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vcge(al, dt, rd, rm, operand);
  }

  void Vcge(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcge(cond, dt, rd, rm, operand);
  }
  void Vcge(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vcge(al, dt, rd, rm, operand);
  }

  void Vcge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcge(cond, dt, rd, rn, rm);
  }
  void Vcge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vcge(al, dt, rd, rn, rm);
  }

  void Vcge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcge(cond, dt, rd, rn, rm);
  }
  void Vcge(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vcge(al, dt, rd, rn, rm);
  }

  void Vcgt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcgt(cond, dt, rd, rm, operand);
  }
  void Vcgt(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vcgt(al, dt, rd, rm, operand);
  }

  void Vcgt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcgt(cond, dt, rd, rm, operand);
  }
  void Vcgt(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vcgt(al, dt, rd, rm, operand);
  }

  void Vcgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcgt(cond, dt, rd, rn, rm);
  }
  void Vcgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vcgt(al, dt, rd, rn, rm);
  }

  void Vcgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcgt(cond, dt, rd, rn, rm);
  }
  void Vcgt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vcgt(al, dt, rd, rn, rm);
  }

  void Vcle(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcle(cond, dt, rd, rm, operand);
  }
  void Vcle(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vcle(al, dt, rd, rm, operand);
  }

  void Vcle(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcle(cond, dt, rd, rm, operand);
  }
  void Vcle(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vcle(al, dt, rd, rm, operand);
  }

  void Vcle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcle(cond, dt, rd, rn, rm);
  }
  void Vcle(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vcle(al, dt, rd, rn, rm);
  }

  void Vcle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcle(cond, dt, rd, rn, rm);
  }
  void Vcle(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vcle(al, dt, rd, rn, rm);
  }

  void Vcls(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcls(cond, dt, rd, rm);
  }
  void Vcls(DataType dt, DRegister rd, DRegister rm) { Vcls(al, dt, rd, rm); }

  void Vcls(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcls(cond, dt, rd, rm);
  }
  void Vcls(DataType dt, QRegister rd, QRegister rm) { Vcls(al, dt, rd, rm); }

  void Vclt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vclt(cond, dt, rd, rm, operand);
  }
  void Vclt(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vclt(al, dt, rd, rm, operand);
  }

  void Vclt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vclt(cond, dt, rd, rm, operand);
  }
  void Vclt(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vclt(al, dt, rd, rm, operand);
  }

  void Vclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vclt(cond, dt, rd, rn, rm);
  }
  void Vclt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vclt(al, dt, rd, rn, rm);
  }

  void Vclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vclt(cond, dt, rd, rn, rm);
  }
  void Vclt(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vclt(al, dt, rd, rn, rm);
  }

  void Vclz(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vclz(cond, dt, rd, rm);
  }
  void Vclz(DataType dt, DRegister rd, DRegister rm) { Vclz(al, dt, rd, rm); }

  void Vclz(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vclz(cond, dt, rd, rm);
  }
  void Vclz(DataType dt, QRegister rd, QRegister rm) { Vclz(al, dt, rd, rm); }

  void Vcmp(Condition cond,
            DataType dt,
            SRegister rd,
            const SOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcmp(cond, dt, rd, operand);
  }
  void Vcmp(DataType dt, SRegister rd, const SOperand& operand) {
    Vcmp(al, dt, rd, operand);
  }

  void Vcmp(Condition cond,
            DataType dt,
            DRegister rd,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcmp(cond, dt, rd, operand);
  }
  void Vcmp(DataType dt, DRegister rd, const DOperand& operand) {
    Vcmp(al, dt, rd, operand);
  }

  void Vcmpe(Condition cond,
             DataType dt,
             SRegister rd,
             const SOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcmpe(cond, dt, rd, operand);
  }
  void Vcmpe(DataType dt, SRegister rd, const SOperand& operand) {
    Vcmpe(al, dt, rd, operand);
  }

  void Vcmpe(Condition cond,
             DataType dt,
             DRegister rd,
             const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcmpe(cond, dt, rd, operand);
  }
  void Vcmpe(DataType dt, DRegister rd, const DOperand& operand) {
    Vcmpe(al, dt, rd, operand);
  }

  void Vcnt(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcnt(cond, dt, rd, rm);
  }
  void Vcnt(DataType dt, DRegister rd, DRegister rm) { Vcnt(al, dt, rd, rm); }

  void Vcnt(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcnt(cond, dt, rd, rm);
  }
  void Vcnt(DataType dt, QRegister rd, QRegister rm) { Vcnt(al, dt, rd, rm); }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            DRegister rd,
            DRegister rm,
            int32_t fbits) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm, fbits);
  }
  void Vcvt(
      DataType dt1, DataType dt2, DRegister rd, DRegister rm, int32_t fbits) {
    Vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void Vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            QRegister rd,
            QRegister rm,
            int32_t fbits) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm, fbits);
  }
  void Vcvt(
      DataType dt1, DataType dt2, QRegister rd, QRegister rm, int32_t fbits) {
    Vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void Vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            SRegister rd,
            SRegister rm,
            int32_t fbits) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm, fbits);
  }
  void Vcvt(
      DataType dt1, DataType dt2, SRegister rd, SRegister rm, int32_t fbits) {
    Vcvt(al, dt1, dt2, rd, rm, fbits);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, DRegister rd, QRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, QRegister rd, DRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvt(cond, dt1, dt2, rd, rm);
  }
  void Vcvt(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvt(al, dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvta(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvta(dt1, dt2, rd, rm);
  }

  void Vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtb(cond, dt1, dt2, rd, rm);
  }
  void Vcvtb(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvtb(al, dt1, dt2, rd, rm);
  }

  void Vcvtb(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtb(cond, dt1, dt2, rd, rm);
  }
  void Vcvtb(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    Vcvtb(al, dt1, dt2, rd, rm);
  }

  void Vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtb(cond, dt1, dt2, rd, rm);
  }
  void Vcvtb(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvtb(al, dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtm(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtm(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtn(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtn(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtp(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vcvtp(dt1, dt2, rd, rm);
  }

  void Vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtr(cond, dt1, dt2, rd, rm);
  }
  void Vcvtr(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvtr(al, dt1, dt2, rd, rm);
  }

  void Vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtr(cond, dt1, dt2, rd, rm);
  }
  void Vcvtr(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvtr(al, dt1, dt2, rd, rm);
  }

  void Vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtt(cond, dt1, dt2, rd, rm);
  }
  void Vcvtt(DataType dt1, DataType dt2, SRegister rd, SRegister rm) {
    Vcvtt(al, dt1, dt2, rd, rm);
  }

  void Vcvtt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtt(cond, dt1, dt2, rd, rm);
  }
  void Vcvtt(DataType dt1, DataType dt2, DRegister rd, SRegister rm) {
    Vcvtt(al, dt1, dt2, rd, rm);
  }

  void Vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vcvtt(cond, dt1, dt2, rd, rm);
  }
  void Vcvtt(DataType dt1, DataType dt2, SRegister rd, DRegister rm) {
    Vcvtt(al, dt1, dt2, rd, rm);
  }

  void Vdiv(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vdiv(cond, dt, rd, rn, rm);
  }
  void Vdiv(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vdiv(al, dt, rd, rn, rm);
  }

  void Vdiv(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vdiv(cond, dt, rd, rn, rm);
  }
  void Vdiv(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vdiv(al, dt, rd, rn, rm);
  }

  void Vdup(Condition cond, DataType dt, QRegister rd, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vdup(cond, dt, rd, rt);
  }
  void Vdup(DataType dt, QRegister rd, Register rt) { Vdup(al, dt, rd, rt); }

  void Vdup(Condition cond, DataType dt, DRegister rd, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vdup(cond, dt, rd, rt);
  }
  void Vdup(DataType dt, DRegister rd, Register rt) { Vdup(al, dt, rd, rt); }

  void Vdup(Condition cond, DataType dt, DRegister rd, DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vdup(cond, dt, rd, rm);
  }
  void Vdup(DataType dt, DRegister rd, DRegisterLane rm) {
    Vdup(al, dt, rd, rm);
  }

  void Vdup(Condition cond, DataType dt, QRegister rd, DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vdup(cond, dt, rd, rm);
  }
  void Vdup(DataType dt, QRegister rd, DRegisterLane rm) {
    Vdup(al, dt, rd, rm);
  }

  void Veor(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    veor(cond, dt, rd, rn, rm);
  }
  void Veor(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Veor(al, dt, rd, rn, rm);
  }
  void Veor(Condition cond, DRegister rd, DRegister rn, DRegister rm) {
    Veor(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Veor(DRegister rd, DRegister rn, DRegister rm) {
    Veor(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Veor(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    veor(cond, dt, rd, rn, rm);
  }
  void Veor(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Veor(al, dt, rd, rn, rm);
  }
  void Veor(Condition cond, QRegister rd, QRegister rn, QRegister rm) {
    Veor(cond, kDataTypeValueNone, rd, rn, rm);
  }
  void Veor(QRegister rd, QRegister rn, QRegister rm) {
    Veor(al, kDataTypeValueNone, rd, rn, rm);
  }

  void Vext(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vext(cond, dt, rd, rn, rm, operand);
  }
  void Vext(DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand) {
    Vext(al, dt, rd, rn, rm, operand);
  }

  void Vext(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vext(cond, dt, rd, rn, rm, operand);
  }
  void Vext(DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand) {
    Vext(al, dt, rd, rn, rm, operand);
  }

  void Vfma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfma(cond, dt, rd, rn, rm);
  }
  void Vfma(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfma(al, dt, rd, rn, rm);
  }

  void Vfma(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfma(cond, dt, rd, rn, rm);
  }
  void Vfma(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vfma(al, dt, rd, rn, rm);
  }

  void Vfma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfma(cond, dt, rd, rn, rm);
  }
  void Vfma(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfma(al, dt, rd, rn, rm);
  }

  void Vfms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfms(cond, dt, rd, rn, rm);
  }
  void Vfms(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfms(al, dt, rd, rn, rm);
  }

  void Vfms(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfms(cond, dt, rd, rn, rm);
  }
  void Vfms(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vfms(al, dt, rd, rn, rm);
  }

  void Vfms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfms(cond, dt, rd, rn, rm);
  }
  void Vfms(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfms(al, dt, rd, rn, rm);
  }

  void Vfnma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfnma(cond, dt, rd, rn, rm);
  }
  void Vfnma(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfnma(al, dt, rd, rn, rm);
  }

  void Vfnma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfnma(cond, dt, rd, rn, rm);
  }
  void Vfnma(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfnma(al, dt, rd, rn, rm);
  }

  void Vfnms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfnms(cond, dt, rd, rn, rm);
  }
  void Vfnms(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vfnms(al, dt, rd, rn, rm);
  }

  void Vfnms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vfnms(cond, dt, rd, rn, rm);
  }
  void Vfnms(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vfnms(al, dt, rd, rn, rm);
  }

  void Vhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vhadd(cond, dt, rd, rn, rm);
  }
  void Vhadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vhadd(al, dt, rd, rn, rm);
  }

  void Vhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vhadd(cond, dt, rd, rn, rm);
  }
  void Vhadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vhadd(al, dt, rd, rn, rm);
  }

  void Vhsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vhsub(cond, dt, rd, rn, rm);
  }
  void Vhsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vhsub(al, dt, rd, rn, rm);
  }

  void Vhsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vhsub(cond, dt, rd, rn, rm);
  }
  void Vhsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vhsub(al, dt, rd, rn, rm);
  }

  void Vld1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vld1(cond, dt, nreglist, operand);
  }
  void Vld1(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld1(al, dt, nreglist, operand);
  }

  void Vld2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vld2(cond, dt, nreglist, operand);
  }
  void Vld2(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld2(al, dt, nreglist, operand);
  }

  void Vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vld3(cond, dt, nreglist, operand);
  }
  void Vld3(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld3(al, dt, nreglist, operand);
  }

  void Vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vld3(cond, dt, nreglist, operand);
  }
  void Vld3(DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    Vld3(al, dt, nreglist, operand);
  }

  void Vld4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vld4(cond, dt, nreglist, operand);
  }
  void Vld4(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vld4(al, dt, nreglist, operand);
  }

  void Vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldm(cond, dt, rn, write_back, dreglist);
  }
  void Vldm(DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vldm(al, dt, rn, write_back, dreglist);
  }
  void Vldm(Condition cond,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vldm(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vldm(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vldm(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldm(cond, dt, rn, write_back, sreglist);
  }
  void Vldm(DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vldm(al, dt, rn, write_back, sreglist);
  }
  void Vldm(Condition cond,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vldm(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vldm(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vldm(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldmdb(cond, dt, rn, write_back, dreglist);
  }
  void Vldmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmdb(al, dt, rn, write_back, dreglist);
  }
  void Vldmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmdb(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vldmdb(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vldmdb(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldmdb(cond, dt, rn, write_back, sreglist);
  }
  void Vldmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmdb(al, dt, rn, write_back, sreglist);
  }
  void Vldmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmdb(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vldmdb(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vldmdb(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldmia(cond, dt, rn, write_back, dreglist);
  }
  void Vldmia(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmia(al, dt, rn, write_back, dreglist);
  }
  void Vldmia(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vldmia(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vldmia(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vldmia(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldmia(cond, dt, rn, write_back, sreglist);
  }
  void Vldmia(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmia(al, dt, rn, write_back, sreglist);
  }
  void Vldmia(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vldmia(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vldmia(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vldmia(al, kDataTypeValueNone, rn, write_back, sreglist);
  }


  void Vldr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldr(cond, dt, rd, operand);
  }
  void Vldr(DataType dt, DRegister rd, const MemOperand& operand) {
    Vldr(al, dt, rd, operand);
  }
  void Vldr(Condition cond, DRegister rd, const MemOperand& operand) {
    Vldr(cond, Untyped64, rd, operand);
  }
  void Vldr(DRegister rd, const MemOperand& operand) {
    Vldr(al, Untyped64, rd, operand);
  }


  void Vldr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vldr(cond, dt, rd, operand);
  }
  void Vldr(DataType dt, SRegister rd, const MemOperand& operand) {
    Vldr(al, dt, rd, operand);
  }
  void Vldr(Condition cond, SRegister rd, const MemOperand& operand) {
    Vldr(cond, Untyped32, rd, operand);
  }
  void Vldr(SRegister rd, const MemOperand& operand) {
    Vldr(al, Untyped32, rd, operand);
  }

  void Vmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmax(cond, dt, rd, rn, rm);
  }
  void Vmax(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmax(al, dt, rd, rn, rm);
  }

  void Vmax(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmax(cond, dt, rd, rn, rm);
  }
  void Vmax(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmax(al, dt, rd, rn, rm);
  }

  void Vmaxnm(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vmaxnm(dt, rd, rn, rm);
  }

  void Vmaxnm(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vmaxnm(dt, rd, rn, rm);
  }

  void Vmaxnm(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vmaxnm(dt, rd, rn, rm);
  }

  void Vmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmin(cond, dt, rd, rn, rm);
  }
  void Vmin(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmin(al, dt, rd, rn, rm);
  }

  void Vmin(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmin(cond, dt, rd, rn, rm);
  }
  void Vmin(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmin(al, dt, rd, rn, rm);
  }

  void Vminnm(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vminnm(dt, rd, rn, rm);
  }

  void Vminnm(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vminnm(dt, rd, rn, rm);
  }

  void Vminnm(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vminnm(dt, rd, rn, rm);
  }

  void Vmla(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmla(cond, dt, rd, rn, rm);
  }
  void Vmla(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vmla(al, dt, rd, rn, rm);
  }

  void Vmlal(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmlal(cond, dt, rd, rn, rm);
  }
  void Vmlal(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    Vmlal(al, dt, rd, rn, rm);
  }

  void Vmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmlal(cond, dt, rd, rn, rm);
  }
  void Vmlal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vmlal(al, dt, rd, rn, rm);
  }

  void Vmls(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmls(cond, dt, rd, rn, rm);
  }
  void Vmls(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vmls(al, dt, rd, rn, rm);
  }

  void Vmlsl(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmlsl(cond, dt, rd, rn, rm);
  }
  void Vmlsl(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    Vmlsl(al, dt, rd, rn, rm);
  }

  void Vmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmlsl(cond, dt, rd, rn, rm);
  }
  void Vmlsl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vmlsl(al, dt, rd, rn, rm);
  }

  void Vmov(Condition cond, Register rt, SRegister rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, rt, rn);
  }
  void Vmov(Register rt, SRegister rn) { Vmov(al, rt, rn); }

  void Vmov(Condition cond, SRegister rn, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, rn, rt);
  }
  void Vmov(SRegister rn, Register rt) { Vmov(al, rn, rt); }

  void Vmov(Condition cond, Register rt, Register rt2, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, rt, rt2, rm);
  }
  void Vmov(Register rt, Register rt2, DRegister rm) { Vmov(al, rt, rt2, rm); }

  void Vmov(Condition cond, DRegister rm, Register rt, Register rt2) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, rm, rt, rt2);
  }
  void Vmov(DRegister rm, Register rt, Register rt2) { Vmov(al, rm, rt, rt2); }

  void Vmov(
      Condition cond, Register rt, Register rt2, SRegister rm, SRegister rm1) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm1));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, rt, rt2, rm, rm1);
  }
  void Vmov(Register rt, Register rt2, SRegister rm, SRegister rm1) {
    Vmov(al, rt, rt2, rm, rm1);
  }

  void Vmov(
      Condition cond, SRegister rm, SRegister rm1, Register rt, Register rt2) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm1));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt2));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, rm, rm1, rt, rt2);
  }
  void Vmov(SRegister rm, SRegister rm1, Register rt, Register rt2) {
    Vmov(al, rm, rm1, rt, rt2);
  }

  void Vmov(Condition cond, DataType dt, DRegisterLane rd, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, dt, rd, rt);
  }
  void Vmov(DataType dt, DRegisterLane rd, Register rt) {
    Vmov(al, dt, rd, rt);
  }
  void Vmov(Condition cond, DRegisterLane rd, Register rt) {
    Vmov(cond, kDataTypeValueNone, rd, rt);
  }
  void Vmov(DRegisterLane rd, Register rt) {
    Vmov(al, kDataTypeValueNone, rd, rt);
  }

  void Vmov(Condition cond,
            DataType dt,
            DRegister rd,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, dt, rd, operand);
  }
  void Vmov(DataType dt, DRegister rd, const DOperand& operand) {
    Vmov(al, dt, rd, operand);
  }

  void Vmov(Condition cond,
            DataType dt,
            QRegister rd,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, dt, rd, operand);
  }
  void Vmov(DataType dt, QRegister rd, const QOperand& operand) {
    Vmov(al, dt, rd, operand);
  }

  void Vmov(Condition cond,
            DataType dt,
            SRegister rd,
            const SOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, dt, rd, operand);
  }
  void Vmov(DataType dt, SRegister rd, const SOperand& operand) {
    Vmov(al, dt, rd, operand);
  }

  void Vmov(Condition cond, DataType dt, Register rt, DRegisterLane rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmov(cond, dt, rt, rn);
  }
  void Vmov(DataType dt, Register rt, DRegisterLane rn) {
    Vmov(al, dt, rt, rn);
  }
  void Vmov(Condition cond, Register rt, DRegisterLane rn) {
    Vmov(cond, kDataTypeValueNone, rt, rn);
  }
  void Vmov(Register rt, DRegisterLane rn) {
    Vmov(al, kDataTypeValueNone, rt, rn);
  }

  void Vmovl(Condition cond, DataType dt, QRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmovl(cond, dt, rd, rm);
  }
  void Vmovl(DataType dt, QRegister rd, DRegister rm) { Vmovl(al, dt, rd, rm); }

  void Vmovn(Condition cond, DataType dt, DRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmovn(cond, dt, rd, rm);
  }
  void Vmovn(DataType dt, DRegister rd, QRegister rm) { Vmovn(al, dt, rd, rm); }

  void Vmrs(Condition cond,
            RegisterOrAPSR_nzcv rt,
            SpecialFPRegister spec_reg) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmrs(cond, rt, spec_reg);
  }
  void Vmrs(RegisterOrAPSR_nzcv rt, SpecialFPRegister spec_reg) {
    Vmrs(al, rt, spec_reg);
  }

  void Vmsr(Condition cond, SpecialFPRegister spec_reg, Register rt) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rt));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmsr(cond, spec_reg, rt);
  }
  void Vmsr(SpecialFPRegister spec_reg, Register rt) { Vmsr(al, spec_reg, rt); }

  void Vmul(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister dm,
            unsigned index) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmul(cond, dt, rd, rn, dm, index);
  }
  void Vmul(
      DataType dt, DRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vmul(al, dt, rd, rn, dm, index);
  }

  void Vmul(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegister dm,
            unsigned index) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmul(cond, dt, rd, rn, dm, index);
  }
  void Vmul(
      DataType dt, QRegister rd, QRegister rn, DRegister dm, unsigned index) {
    Vmul(al, dt, rd, rn, dm, index);
  }

  void Vmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmul(cond, dt, rd, rn, rm);
  }
  void Vmul(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vmul(al, dt, rd, rn, rm);
  }

  void Vmul(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmul(cond, dt, rd, rn, rm);
  }
  void Vmul(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vmul(al, dt, rd, rn, rm);
  }

  void Vmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmul(cond, dt, rd, rn, rm);
  }
  void Vmul(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vmul(al, dt, rd, rn, rm);
  }

  void Vmull(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegister dm,
             unsigned index) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmull(cond, dt, rd, rn, dm, index);
  }
  void Vmull(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vmull(al, dt, rd, rn, dm, index);
  }

  void Vmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmull(cond, dt, rd, rn, rm);
  }
  void Vmull(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vmull(al, dt, rd, rn, rm);
  }

  void Vmvn(Condition cond,
            DataType dt,
            DRegister rd,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmvn(cond, dt, rd, operand);
  }
  void Vmvn(DataType dt, DRegister rd, const DOperand& operand) {
    Vmvn(al, dt, rd, operand);
  }

  void Vmvn(Condition cond,
            DataType dt,
            QRegister rd,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vmvn(cond, dt, rd, operand);
  }
  void Vmvn(DataType dt, QRegister rd, const QOperand& operand) {
    Vmvn(al, dt, rd, operand);
  }

  void Vneg(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vneg(cond, dt, rd, rm);
  }
  void Vneg(DataType dt, DRegister rd, DRegister rm) { Vneg(al, dt, rd, rm); }

  void Vneg(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vneg(cond, dt, rd, rm);
  }
  void Vneg(DataType dt, QRegister rd, QRegister rm) { Vneg(al, dt, rd, rm); }

  void Vneg(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vneg(cond, dt, rd, rm);
  }
  void Vneg(DataType dt, SRegister rd, SRegister rm) { Vneg(al, dt, rd, rm); }

  void Vnmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vnmla(cond, dt, rd, rn, rm);
  }
  void Vnmla(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vnmla(al, dt, rd, rn, rm);
  }

  void Vnmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vnmla(cond, dt, rd, rn, rm);
  }
  void Vnmla(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vnmla(al, dt, rd, rn, rm);
  }

  void Vnmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vnmls(cond, dt, rd, rn, rm);
  }
  void Vnmls(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vnmls(al, dt, rd, rn, rm);
  }

  void Vnmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vnmls(cond, dt, rd, rn, rm);
  }
  void Vnmls(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vnmls(al, dt, rd, rn, rm);
  }

  void Vnmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vnmul(cond, dt, rd, rn, rm);
  }
  void Vnmul(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vnmul(al, dt, rd, rn, rm);
  }

  void Vnmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vnmul(cond, dt, rd, rn, rm);
  }
  void Vnmul(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vnmul(al, dt, rd, rn, rm);
  }

  void Vorn(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vorn(cond, dt, rd, rn, operand);
  }
  void Vorn(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vorn(al, dt, rd, rn, operand);
  }

  void Vorn(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vorn(cond, dt, rd, rn, operand);
  }
  void Vorn(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vorn(al, dt, rd, rn, operand);
  }

  void Vorr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vorr(cond, dt, rd, rn, operand);
  }
  void Vorr(DataType dt, DRegister rd, DRegister rn, const DOperand& operand) {
    Vorr(al, dt, rd, rn, operand);
  }
  void Vorr(Condition cond,
            DRegister rd,
            DRegister rn,
            const DOperand& operand) {
    Vorr(cond, kDataTypeValueNone, rd, rn, operand);
  }
  void Vorr(DRegister rd, DRegister rn, const DOperand& operand) {
    Vorr(al, kDataTypeValueNone, rd, rn, operand);
  }

  void Vorr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vorr(cond, dt, rd, rn, operand);
  }
  void Vorr(DataType dt, QRegister rd, QRegister rn, const QOperand& operand) {
    Vorr(al, dt, rd, rn, operand);
  }
  void Vorr(Condition cond,
            QRegister rd,
            QRegister rn,
            const QOperand& operand) {
    Vorr(cond, kDataTypeValueNone, rd, rn, operand);
  }
  void Vorr(QRegister rd, QRegister rn, const QOperand& operand) {
    Vorr(al, kDataTypeValueNone, rd, rn, operand);
  }

  void Vpadal(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpadal(cond, dt, rd, rm);
  }
  void Vpadal(DataType dt, DRegister rd, DRegister rm) {
    Vpadal(al, dt, rd, rm);
  }

  void Vpadal(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpadal(cond, dt, rd, rm);
  }
  void Vpadal(DataType dt, QRegister rd, QRegister rm) {
    Vpadal(al, dt, rd, rm);
  }

  void Vpadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpadd(cond, dt, rd, rn, rm);
  }
  void Vpadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vpadd(al, dt, rd, rn, rm);
  }

  void Vpaddl(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpaddl(cond, dt, rd, rm);
  }
  void Vpaddl(DataType dt, DRegister rd, DRegister rm) {
    Vpaddl(al, dt, rd, rm);
  }

  void Vpaddl(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpaddl(cond, dt, rd, rm);
  }
  void Vpaddl(DataType dt, QRegister rd, QRegister rm) {
    Vpaddl(al, dt, rd, rm);
  }

  void Vpmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpmax(cond, dt, rd, rn, rm);
  }
  void Vpmax(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vpmax(al, dt, rd, rn, rm);
  }

  void Vpmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpmin(cond, dt, rd, rn, rm);
  }
  void Vpmin(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vpmin(al, dt, rd, rn, rm);
  }

  void Vpop(Condition cond, DataType dt, DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpop(cond, dt, dreglist);
  }
  void Vpop(DataType dt, DRegisterList dreglist) { Vpop(al, dt, dreglist); }
  void Vpop(Condition cond, DRegisterList dreglist) {
    Vpop(cond, kDataTypeValueNone, dreglist);
  }
  void Vpop(DRegisterList dreglist) { Vpop(al, kDataTypeValueNone, dreglist); }

  void Vpop(Condition cond, DataType dt, SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpop(cond, dt, sreglist);
  }
  void Vpop(DataType dt, SRegisterList sreglist) { Vpop(al, dt, sreglist); }
  void Vpop(Condition cond, SRegisterList sreglist) {
    Vpop(cond, kDataTypeValueNone, sreglist);
  }
  void Vpop(SRegisterList sreglist) { Vpop(al, kDataTypeValueNone, sreglist); }

  void Vpush(Condition cond, DataType dt, DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpush(cond, dt, dreglist);
  }
  void Vpush(DataType dt, DRegisterList dreglist) { Vpush(al, dt, dreglist); }
  void Vpush(Condition cond, DRegisterList dreglist) {
    Vpush(cond, kDataTypeValueNone, dreglist);
  }
  void Vpush(DRegisterList dreglist) {
    Vpush(al, kDataTypeValueNone, dreglist);
  }

  void Vpush(Condition cond, DataType dt, SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vpush(cond, dt, sreglist);
  }
  void Vpush(DataType dt, SRegisterList sreglist) { Vpush(al, dt, sreglist); }
  void Vpush(Condition cond, SRegisterList sreglist) {
    Vpush(cond, kDataTypeValueNone, sreglist);
  }
  void Vpush(SRegisterList sreglist) {
    Vpush(al, kDataTypeValueNone, sreglist);
  }

  void Vqabs(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqabs(cond, dt, rd, rm);
  }
  void Vqabs(DataType dt, DRegister rd, DRegister rm) { Vqabs(al, dt, rd, rm); }

  void Vqabs(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqabs(cond, dt, rd, rm);
  }
  void Vqabs(DataType dt, QRegister rd, QRegister rm) { Vqabs(al, dt, rd, rm); }

  void Vqadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqadd(cond, dt, rd, rn, rm);
  }
  void Vqadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqadd(al, dt, rd, rn, rm);
  }

  void Vqadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqadd(cond, dt, rd, rn, rm);
  }
  void Vqadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqadd(al, dt, rd, rn, rm);
  }

  void Vqdmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmlal(cond, dt, rd, rn, rm);
  }
  void Vqdmlal(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vqdmlal(al, dt, rd, rn, rm);
  }

  void Vqdmlal(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmlal(cond, dt, rd, rn, dm, index);
  }
  void Vqdmlal(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vqdmlal(al, dt, rd, rn, dm, index);
  }

  void Vqdmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmlsl(cond, dt, rd, rn, rm);
  }
  void Vqdmlsl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vqdmlsl(al, dt, rd, rn, rm);
  }

  void Vqdmlsl(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmlsl(cond, dt, rd, rn, dm, index);
  }
  void Vqdmlsl(
      DataType dt, QRegister rd, DRegister rn, DRegister dm, unsigned index) {
    Vqdmlsl(al, dt, rd, rn, dm, index);
  }

  void Vqdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmulh(Condition cond,
               DataType dt,
               DRegister rd,
               DRegister rn,
               DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmulh(Condition cond,
               DataType dt,
               QRegister rd,
               QRegister rn,
               DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmulh(cond, dt, rd, rn, rm);
  }
  void Vqdmulh(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vqdmulh(al, dt, rd, rn, rm);
  }

  void Vqdmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmull(cond, dt, rd, rn, rm);
  }
  void Vqdmull(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vqdmull(al, dt, rd, rn, rm);
  }

  void Vqdmull(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqdmull(cond, dt, rd, rn, rm);
  }
  void Vqdmull(DataType dt, QRegister rd, DRegister rn, DRegisterLane rm) {
    Vqdmull(al, dt, rd, rn, rm);
  }

  void Vqmovn(Condition cond, DataType dt, DRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqmovn(cond, dt, rd, rm);
  }
  void Vqmovn(DataType dt, DRegister rd, QRegister rm) {
    Vqmovn(al, dt, rd, rm);
  }

  void Vqmovun(Condition cond, DataType dt, DRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqmovun(cond, dt, rd, rm);
  }
  void Vqmovun(DataType dt, DRegister rd, QRegister rm) {
    Vqmovun(al, dt, rd, rm);
  }

  void Vqneg(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqneg(cond, dt, rd, rm);
  }
  void Vqneg(DataType dt, DRegister rd, DRegister rm) { Vqneg(al, dt, rd, rm); }

  void Vqneg(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqneg(cond, dt, rd, rm);
  }
  void Vqneg(DataType dt, QRegister rd, QRegister rm) { Vqneg(al, dt, rd, rm); }

  void Vqrdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrdmulh(Condition cond,
                DataType dt,
                DRegister rd,
                DRegister rn,
                DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, DRegister rd, DRegister rn, DRegisterLane rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrdmulh(Condition cond,
                DataType dt,
                QRegister rd,
                QRegister rn,
                DRegisterLane rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrdmulh(cond, dt, rd, rn, rm);
  }
  void Vqrdmulh(DataType dt, QRegister rd, QRegister rn, DRegisterLane rm) {
    Vqrdmulh(al, dt, rd, rn, rm);
  }

  void Vqrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrshl(cond, dt, rd, rm, rn);
  }
  void Vqrshl(DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    Vqrshl(al, dt, rd, rm, rn);
  }

  void Vqrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrshl(cond, dt, rd, rm, rn);
  }
  void Vqrshl(DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    Vqrshl(al, dt, rd, rm, rn);
  }

  void Vqrshrn(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrshrn(cond, dt, rd, rm, operand);
  }
  void Vqrshrn(DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    Vqrshrn(al, dt, rd, rm, operand);
  }

  void Vqrshrun(Condition cond,
                DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqrshrun(cond, dt, rd, rm, operand);
  }
  void Vqrshrun(DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand) {
    Vqrshrun(al, dt, rd, rm, operand);
  }

  void Vqshl(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqshl(cond, dt, rd, rm, operand);
  }
  void Vqshl(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vqshl(al, dt, rd, rm, operand);
  }

  void Vqshl(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqshl(cond, dt, rd, rm, operand);
  }
  void Vqshl(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vqshl(al, dt, rd, rm, operand);
  }

  void Vqshlu(Condition cond,
              DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqshlu(cond, dt, rd, rm, operand);
  }
  void Vqshlu(DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand) {
    Vqshlu(al, dt, rd, rm, operand);
  }

  void Vqshlu(Condition cond,
              DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqshlu(cond, dt, rd, rm, operand);
  }
  void Vqshlu(DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand) {
    Vqshlu(al, dt, rd, rm, operand);
  }

  void Vqshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqshrn(cond, dt, rd, rm, operand);
  }
  void Vqshrn(DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    Vqshrn(al, dt, rd, rm, operand);
  }

  void Vqshrun(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqshrun(cond, dt, rd, rm, operand);
  }
  void Vqshrun(DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand) {
    Vqshrun(al, dt, rd, rm, operand);
  }

  void Vqsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqsub(cond, dt, rd, rn, rm);
  }
  void Vqsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vqsub(al, dt, rd, rn, rm);
  }

  void Vqsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vqsub(cond, dt, rd, rn, rm);
  }
  void Vqsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vqsub(al, dt, rd, rn, rm);
  }

  void Vraddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vraddhn(cond, dt, rd, rn, rm);
  }
  void Vraddhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vraddhn(al, dt, rd, rn, rm);
  }

  void Vrecpe(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrecpe(cond, dt, rd, rm);
  }
  void Vrecpe(DataType dt, DRegister rd, DRegister rm) {
    Vrecpe(al, dt, rd, rm);
  }

  void Vrecpe(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrecpe(cond, dt, rd, rm);
  }
  void Vrecpe(DataType dt, QRegister rd, QRegister rm) {
    Vrecpe(al, dt, rd, rm);
  }

  void Vrecps(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrecps(cond, dt, rd, rn, rm);
  }
  void Vrecps(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vrecps(al, dt, rd, rn, rm);
  }

  void Vrecps(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrecps(cond, dt, rd, rn, rm);
  }
  void Vrecps(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vrecps(al, dt, rd, rn, rm);
  }

  void Vrev16(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrev16(cond, dt, rd, rm);
  }
  void Vrev16(DataType dt, DRegister rd, DRegister rm) {
    Vrev16(al, dt, rd, rm);
  }

  void Vrev16(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrev16(cond, dt, rd, rm);
  }
  void Vrev16(DataType dt, QRegister rd, QRegister rm) {
    Vrev16(al, dt, rd, rm);
  }

  void Vrev32(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrev32(cond, dt, rd, rm);
  }
  void Vrev32(DataType dt, DRegister rd, DRegister rm) {
    Vrev32(al, dt, rd, rm);
  }

  void Vrev32(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrev32(cond, dt, rd, rm);
  }
  void Vrev32(DataType dt, QRegister rd, QRegister rm) {
    Vrev32(al, dt, rd, rm);
  }

  void Vrev64(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrev64(cond, dt, rd, rm);
  }
  void Vrev64(DataType dt, DRegister rd, DRegister rm) {
    Vrev64(al, dt, rd, rm);
  }

  void Vrev64(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrev64(cond, dt, rd, rm);
  }
  void Vrev64(DataType dt, QRegister rd, QRegister rm) {
    Vrev64(al, dt, rd, rm);
  }

  void Vrhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrhadd(cond, dt, rd, rn, rm);
  }
  void Vrhadd(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vrhadd(al, dt, rd, rn, rm);
  }

  void Vrhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrhadd(cond, dt, rd, rn, rm);
  }
  void Vrhadd(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vrhadd(al, dt, rd, rn, rm);
  }

  void Vrinta(DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrinta(dt, rd, rm);
  }

  void Vrinta(DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrinta(dt, rd, rm);
  }

  void Vrinta(DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrinta(dt, rd, rm);
  }

  void Vrintm(DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintm(dt, rd, rm);
  }

  void Vrintm(DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintm(dt, rd, rm);
  }

  void Vrintm(DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintm(dt, rd, rm);
  }

  void Vrintn(DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintn(dt, rd, rm);
  }

  void Vrintn(DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintn(dt, rd, rm);
  }

  void Vrintn(DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintn(dt, rd, rm);
  }

  void Vrintp(DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintp(dt, rd, rm);
  }

  void Vrintp(DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintp(dt, rd, rm);
  }

  void Vrintp(DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintp(dt, rd, rm);
  }

  void Vrintr(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrintr(cond, dt, rd, rm);
  }
  void Vrintr(DataType dt, SRegister rd, SRegister rm) {
    Vrintr(al, dt, rd, rm);
  }

  void Vrintr(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrintr(cond, dt, rd, rm);
  }
  void Vrintr(DataType dt, DRegister rd, DRegister rm) {
    Vrintr(al, dt, rd, rm);
  }

  void Vrintx(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrintx(cond, dt, rd, rm);
  }
  void Vrintx(DataType dt, DRegister rd, DRegister rm) {
    Vrintx(al, dt, rd, rm);
  }

  void Vrintx(DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintx(dt, rd, rm);
  }

  void Vrintx(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrintx(cond, dt, rd, rm);
  }
  void Vrintx(DataType dt, SRegister rd, SRegister rm) {
    Vrintx(al, dt, rd, rm);
  }

  void Vrintz(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrintz(cond, dt, rd, rm);
  }
  void Vrintz(DataType dt, DRegister rd, DRegister rm) {
    Vrintz(al, dt, rd, rm);
  }

  void Vrintz(DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vrintz(dt, rd, rm);
  }

  void Vrintz(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrintz(cond, dt, rd, rm);
  }
  void Vrintz(DataType dt, SRegister rd, SRegister rm) {
    Vrintz(al, dt, rd, rm);
  }

  void Vrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrshl(cond, dt, rd, rm, rn);
  }
  void Vrshl(DataType dt, DRegister rd, DRegister rm, DRegister rn) {
    Vrshl(al, dt, rd, rm, rn);
  }

  void Vrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrshl(cond, dt, rd, rm, rn);
  }
  void Vrshl(DataType dt, QRegister rd, QRegister rm, QRegister rn) {
    Vrshl(al, dt, rd, rm, rn);
  }

  void Vrshr(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrshr(cond, dt, rd, rm, operand);
  }
  void Vrshr(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vrshr(al, dt, rd, rm, operand);
  }

  void Vrshr(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrshr(cond, dt, rd, rm, operand);
  }
  void Vrshr(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vrshr(al, dt, rd, rm, operand);
  }

  void Vrshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrshrn(cond, dt, rd, rm, operand);
  }
  void Vrshrn(DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand) {
    Vrshrn(al, dt, rd, rm, operand);
  }

  void Vrsqrte(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsqrte(cond, dt, rd, rm);
  }
  void Vrsqrte(DataType dt, DRegister rd, DRegister rm) {
    Vrsqrte(al, dt, rd, rm);
  }

  void Vrsqrte(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsqrte(cond, dt, rd, rm);
  }
  void Vrsqrte(DataType dt, QRegister rd, QRegister rm) {
    Vrsqrte(al, dt, rd, rm);
  }

  void Vrsqrts(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsqrts(cond, dt, rd, rn, rm);
  }
  void Vrsqrts(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vrsqrts(al, dt, rd, rn, rm);
  }

  void Vrsqrts(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsqrts(cond, dt, rd, rn, rm);
  }
  void Vrsqrts(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vrsqrts(al, dt, rd, rn, rm);
  }

  void Vrsra(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsra(cond, dt, rd, rm, operand);
  }
  void Vrsra(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vrsra(al, dt, rd, rm, operand);
  }

  void Vrsra(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsra(cond, dt, rd, rm, operand);
  }
  void Vrsra(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vrsra(al, dt, rd, rm, operand);
  }

  void Vrsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vrsubhn(cond, dt, rd, rn, rm);
  }
  void Vrsubhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vrsubhn(al, dt, rd, rn, rm);
  }

  void Vseleq(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vseleq(dt, rd, rn, rm);
  }

  void Vseleq(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vseleq(dt, rd, rn, rm);
  }

  void Vselge(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vselge(dt, rd, rn, rm);
  }

  void Vselge(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vselge(dt, rd, rn, rm);
  }

  void Vselgt(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vselgt(dt, rd, rn, rm);
  }

  void Vselgt(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vselgt(dt, rd, rn, rm);
  }

  void Vselvs(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vselvs(dt, rd, rn, rm);
  }

  void Vselvs(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    vselvs(dt, rd, rn, rm);
  }

  void Vshl(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vshl(cond, dt, rd, rm, operand);
  }
  void Vshl(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vshl(al, dt, rd, rm, operand);
  }

  void Vshl(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vshl(cond, dt, rd, rm, operand);
  }
  void Vshl(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vshl(al, dt, rd, rm, operand);
  }

  void Vshll(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rm,
             const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vshll(cond, dt, rd, rm, operand);
  }
  void Vshll(DataType dt, QRegister rd, DRegister rm, const DOperand& operand) {
    Vshll(al, dt, rd, rm, operand);
  }

  void Vshr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vshr(cond, dt, rd, rm, operand);
  }
  void Vshr(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vshr(al, dt, rd, rm, operand);
  }

  void Vshr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vshr(cond, dt, rd, rm, operand);
  }
  void Vshr(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vshr(al, dt, rd, rm, operand);
  }

  void Vshrn(Condition cond,
             DataType dt,
             DRegister rd,
             QRegister rm,
             const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vshrn(cond, dt, rd, rm, operand);
  }
  void Vshrn(DataType dt, DRegister rd, QRegister rm, const QOperand& operand) {
    Vshrn(al, dt, rd, rm, operand);
  }

  void Vsli(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsli(cond, dt, rd, rm, operand);
  }
  void Vsli(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vsli(al, dt, rd, rm, operand);
  }

  void Vsli(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsli(cond, dt, rd, rm, operand);
  }
  void Vsli(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vsli(al, dt, rd, rm, operand);
  }

  void Vsqrt(Condition cond, DataType dt, SRegister rd, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsqrt(cond, dt, rd, rm);
  }
  void Vsqrt(DataType dt, SRegister rd, SRegister rm) { Vsqrt(al, dt, rd, rm); }

  void Vsqrt(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsqrt(cond, dt, rd, rm);
  }
  void Vsqrt(DataType dt, DRegister rd, DRegister rm) { Vsqrt(al, dt, rd, rm); }

  void Vsra(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsra(cond, dt, rd, rm, operand);
  }
  void Vsra(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vsra(al, dt, rd, rm, operand);
  }

  void Vsra(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsra(cond, dt, rd, rm, operand);
  }
  void Vsra(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vsra(al, dt, rd, rm, operand);
  }

  void Vsri(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsri(cond, dt, rd, rm, operand);
  }
  void Vsri(DataType dt, DRegister rd, DRegister rm, const DOperand& operand) {
    Vsri(al, dt, rd, rm, operand);
  }

  void Vsri(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsri(cond, dt, rd, rm, operand);
  }
  void Vsri(DataType dt, QRegister rd, QRegister rm, const QOperand& operand) {
    Vsri(al, dt, rd, rm, operand);
  }

  void Vst1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vst1(cond, dt, nreglist, operand);
  }
  void Vst1(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst1(al, dt, nreglist, operand);
  }

  void Vst2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vst2(cond, dt, nreglist, operand);
  }
  void Vst2(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst2(al, dt, nreglist, operand);
  }

  void Vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vst3(cond, dt, nreglist, operand);
  }
  void Vst3(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst3(al, dt, nreglist, operand);
  }

  void Vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vst3(cond, dt, nreglist, operand);
  }
  void Vst3(DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand) {
    Vst3(al, dt, nreglist, operand);
  }

  void Vst4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vst4(cond, dt, nreglist, operand);
  }
  void Vst4(DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand) {
    Vst4(al, dt, nreglist, operand);
  }

  void Vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstm(cond, dt, rn, write_back, dreglist);
  }
  void Vstm(DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vstm(al, dt, rn, write_back, dreglist);
  }
  void Vstm(Condition cond,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist) {
    Vstm(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vstm(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vstm(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstm(cond, dt, rn, write_back, sreglist);
  }
  void Vstm(DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vstm(al, dt, rn, write_back, sreglist);
  }
  void Vstm(Condition cond,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist) {
    Vstm(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vstm(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vstm(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstmdb(cond, dt, rn, write_back, dreglist);
  }
  void Vstmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmdb(al, dt, rn, write_back, dreglist);
  }
  void Vstmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmdb(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vstmdb(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vstmdb(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstmdb(cond, dt, rn, write_back, sreglist);
  }
  void Vstmdb(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmdb(al, dt, rn, write_back, sreglist);
  }
  void Vstmdb(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmdb(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vstmdb(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vstmdb(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(dreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstmia(cond, dt, rn, write_back, dreglist);
  }
  void Vstmia(DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmia(al, dt, rn, write_back, dreglist);
  }
  void Vstmia(Condition cond,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist) {
    Vstmia(cond, kDataTypeValueNone, rn, write_back, dreglist);
  }
  void Vstmia(Register rn, WriteBack write_back, DRegisterList dreglist) {
    Vstmia(al, kDataTypeValueNone, rn, write_back, dreglist);
  }

  void Vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(sreglist));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstmia(cond, dt, rn, write_back, sreglist);
  }
  void Vstmia(DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmia(al, dt, rn, write_back, sreglist);
  }
  void Vstmia(Condition cond,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist) {
    Vstmia(cond, kDataTypeValueNone, rn, write_back, sreglist);
  }
  void Vstmia(Register rn, WriteBack write_back, SRegisterList sreglist) {
    Vstmia(al, kDataTypeValueNone, rn, write_back, sreglist);
  }

  void Vstr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstr(cond, dt, rd, operand);
  }
  void Vstr(DataType dt, DRegister rd, const MemOperand& operand) {
    Vstr(al, dt, rd, operand);
  }
  void Vstr(Condition cond, DRegister rd, const MemOperand& operand) {
    Vstr(cond, Untyped64, rd, operand);
  }
  void Vstr(DRegister rd, const MemOperand& operand) {
    Vstr(al, Untyped64, rd, operand);
  }

  void Vstr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(operand));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vstr(cond, dt, rd, operand);
  }
  void Vstr(DataType dt, SRegister rd, const MemOperand& operand) {
    Vstr(al, dt, rd, operand);
  }
  void Vstr(Condition cond, SRegister rd, const MemOperand& operand) {
    Vstr(cond, Untyped32, rd, operand);
  }
  void Vstr(SRegister rd, const MemOperand& operand) {
    Vstr(al, Untyped32, rd, operand);
  }

  void Vsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsub(cond, dt, rd, rn, rm);
  }
  void Vsub(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vsub(al, dt, rd, rn, rm);
  }

  void Vsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsub(cond, dt, rd, rn, rm);
  }
  void Vsub(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vsub(al, dt, rd, rn, rm);
  }

  void Vsub(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsub(cond, dt, rd, rn, rm);
  }
  void Vsub(DataType dt, SRegister rd, SRegister rn, SRegister rm) {
    Vsub(al, dt, rd, rn, rm);
  }

  void Vsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsubhn(cond, dt, rd, rn, rm);
  }
  void Vsubhn(DataType dt, DRegister rd, QRegister rn, QRegister rm) {
    Vsubhn(al, dt, rd, rn, rm);
  }

  void Vsubl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsubl(cond, dt, rd, rn, rm);
  }
  void Vsubl(DataType dt, QRegister rd, DRegister rn, DRegister rm) {
    Vsubl(al, dt, rd, rn, rm);
  }

  void Vsubw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vsubw(cond, dt, rd, rn, rm);
  }
  void Vsubw(DataType dt, QRegister rd, QRegister rn, DRegister rm) {
    Vsubw(al, dt, rd, rn, rm);
  }

  void Vswp(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vswp(cond, dt, rd, rm);
  }
  void Vswp(DataType dt, DRegister rd, DRegister rm) { Vswp(al, dt, rd, rm); }
  void Vswp(Condition cond, DRegister rd, DRegister rm) {
    Vswp(cond, kDataTypeValueNone, rd, rm);
  }
  void Vswp(DRegister rd, DRegister rm) {
    Vswp(al, kDataTypeValueNone, rd, rm);
  }

  void Vswp(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vswp(cond, dt, rd, rm);
  }
  void Vswp(DataType dt, QRegister rd, QRegister rm) { Vswp(al, dt, rd, rm); }
  void Vswp(Condition cond, QRegister rd, QRegister rm) {
    Vswp(cond, kDataTypeValueNone, rd, rm);
  }
  void Vswp(QRegister rd, QRegister rm) {
    Vswp(al, kDataTypeValueNone, rd, rm);
  }

  void Vtbl(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vtbl(cond, dt, rd, nreglist, rm);
  }
  void Vtbl(DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    Vtbl(al, dt, rd, nreglist, rm);
  }

  void Vtbx(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(nreglist));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vtbx(cond, dt, rd, nreglist, rm);
  }
  void Vtbx(DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm) {
    Vtbx(al, dt, rd, nreglist, rm);
  }

  void Vtrn(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vtrn(cond, dt, rd, rm);
  }
  void Vtrn(DataType dt, DRegister rd, DRegister rm) { Vtrn(al, dt, rd, rm); }

  void Vtrn(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vtrn(cond, dt, rd, rm);
  }
  void Vtrn(DataType dt, QRegister rd, QRegister rm) { Vtrn(al, dt, rd, rm); }

  void Vtst(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vtst(cond, dt, rd, rn, rm);
  }
  void Vtst(DataType dt, DRegister rd, DRegister rn, DRegister rm) {
    Vtst(al, dt, rd, rn, rm);
  }

  void Vtst(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rn));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vtst(cond, dt, rd, rn, rm);
  }
  void Vtst(DataType dt, QRegister rd, QRegister rn, QRegister rm) {
    Vtst(al, dt, rd, rn, rm);
  }

  void Vuzp(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vuzp(cond, dt, rd, rm);
  }
  void Vuzp(DataType dt, DRegister rd, DRegister rm) { Vuzp(al, dt, rd, rm); }

  void Vuzp(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vuzp(cond, dt, rd, rm);
  }
  void Vuzp(DataType dt, QRegister rd, QRegister rm) { Vuzp(al, dt, rd, rm); }

  void Vzip(Condition cond, DataType dt, DRegister rd, DRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vzip(cond, dt, rd, rm);
  }
  void Vzip(DataType dt, DRegister rd, DRegister rm) { Vzip(al, dt, rd, rm); }

  void Vzip(Condition cond, DataType dt, QRegister rd, QRegister rm) {
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rd));
    VIXL_ASSERT(!AliasesAvailableScratchRegister(rm));
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    vzip(cond, dt, rd, rm);
  }
  void Vzip(DataType dt, QRegister rd, QRegister rm) { Vzip(al, dt, rd, rm); }

  void Yield(Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(OutsideITBlock());
    MacroEmissionCheckScope guard(this);
    ITScope it_scope(this, &cond, guard);
    yield(cond);
  }
  void Yield() { Yield(al); }
  void Vabs(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vabs(cond, F32, rd.S(), rm.S());
    } else {
      Vabs(cond, F64, rd.D(), rm.D());
    }
  }
  void Vabs(VRegister rd, VRegister rm) { Vabs(al, rd, rm); }
  void Vadd(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vadd(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vadd(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vadd(VRegister rd, VRegister rn, VRegister rm) { Vadd(al, rd, rn, rm); }
  void Vcmp(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vcmp(cond, F32, rd.S(), rm.S());
    } else {
      Vcmp(cond, F64, rd.D(), rm.D());
    }
  }
  void Vcmp(VRegister rd, VRegister rm) { Vcmp(al, rd, rm); }
  void Vcmpe(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vcmpe(cond, F32, rd.S(), rm.S());
    } else {
      Vcmpe(cond, F64, rd.D(), rm.D());
    }
  }
  void Vcmpe(VRegister rd, VRegister rm) { Vcmpe(al, rd, rm); }
  void Vdiv(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vdiv(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vdiv(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vdiv(VRegister rd, VRegister rn, VRegister rm) { Vdiv(al, rd, rn, rm); }
  void Vfma(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vfma(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vfma(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vfma(VRegister rd, VRegister rn, VRegister rm) { Vfma(al, rd, rn, rm); }
  void Vfms(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vfms(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vfms(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vfms(VRegister rd, VRegister rn, VRegister rm) { Vfms(al, rd, rn, rm); }
  void Vfnma(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vfnma(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vfnma(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vfnma(VRegister rd, VRegister rn, VRegister rm) {
    Vfnma(al, rd, rn, rm);
  }
  void Vfnms(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vfnms(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vfnms(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vfnms(VRegister rd, VRegister rn, VRegister rm) {
    Vfnms(al, rd, rn, rm);
  }
  void Vmaxnm(VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vmaxnm(F32, rd.S(), rn.S(), rm.S());
    } else {
      Vmaxnm(F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vminnm(VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vminnm(F32, rd.S(), rn.S(), rm.S());
    } else {
      Vminnm(F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vmla(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vmla(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vmla(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vmla(VRegister rd, VRegister rn, VRegister rm) { Vmla(al, rd, rn, rm); }
  void Vmls(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vmls(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vmls(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vmls(VRegister rd, VRegister rn, VRegister rm) { Vmls(al, rd, rn, rm); }
  void Vmov(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vmov(cond, F32, rd.S(), rm.S());
    } else {
      Vmov(cond, F64, rd.D(), rm.D());
    }
  }
  void Vmov(VRegister rd, VRegister rm) { Vmov(al, rd, rm); }
  void Vmul(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vmul(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vmul(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vmul(VRegister rd, VRegister rn, VRegister rm) { Vmul(al, rd, rn, rm); }
  void Vneg(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vneg(cond, F32, rd.S(), rm.S());
    } else {
      Vneg(cond, F64, rd.D(), rm.D());
    }
  }
  void Vneg(VRegister rd, VRegister rm) { Vneg(al, rd, rm); }
  void Vnmla(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vnmla(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vnmla(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vnmla(VRegister rd, VRegister rn, VRegister rm) {
    Vnmla(al, rd, rn, rm);
  }
  void Vnmls(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vnmls(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vnmls(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vnmls(VRegister rd, VRegister rn, VRegister rm) {
    Vnmls(al, rd, rn, rm);
  }
  void Vnmul(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vnmul(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vnmul(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vnmul(VRegister rd, VRegister rn, VRegister rm) {
    Vnmul(al, rd, rn, rm);
  }
  void Vrinta(VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrinta(F32, rd.S(), rm.S());
    } else {
      Vrinta(F64, rd.D(), rm.D());
    }
  }
  void Vrintm(VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrintm(F32, rd.S(), rm.S());
    } else {
      Vrintm(F64, rd.D(), rm.D());
    }
  }
  void Vrintn(VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrintn(F32, rd.S(), rm.S());
    } else {
      Vrintn(F64, rd.D(), rm.D());
    }
  }
  void Vrintp(VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrintp(F32, rd.S(), rm.S());
    } else {
      Vrintp(F64, rd.D(), rm.D());
    }
  }
  void Vrintr(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrintr(cond, F32, rd.S(), rm.S());
    } else {
      Vrintr(cond, F64, rd.D(), rm.D());
    }
  }
  void Vrintr(VRegister rd, VRegister rm) { Vrintr(al, rd, rm); }
  void Vrintx(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrintx(cond, F32, rd.S(), rm.S());
    } else {
      Vrintx(cond, F64, rd.D(), rm.D());
    }
  }
  void Vrintx(VRegister rd, VRegister rm) { Vrintx(al, rd, rm); }
  void Vrintz(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vrintz(cond, F32, rd.S(), rm.S());
    } else {
      Vrintz(cond, F64, rd.D(), rm.D());
    }
  }
  void Vrintz(VRegister rd, VRegister rm) { Vrintz(al, rd, rm); }
  void Vseleq(VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vseleq(F32, rd.S(), rn.S(), rm.S());
    } else {
      Vseleq(F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vselge(VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vselge(F32, rd.S(), rn.S(), rm.S());
    } else {
      Vselge(F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vselgt(VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vselgt(F32, rd.S(), rn.S(), rm.S());
    } else {
      Vselgt(F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vselvs(VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vselvs(F32, rd.S(), rn.S(), rm.S());
    } else {
      Vselvs(F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vsqrt(Condition cond, VRegister rd, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vsqrt(cond, F32, rd.S(), rm.S());
    } else {
      Vsqrt(cond, F64, rd.D(), rm.D());
    }
  }
  void Vsqrt(VRegister rd, VRegister rm) { Vsqrt(al, rd, rm); }
  void Vsub(Condition cond, VRegister rd, VRegister rn, VRegister rm) {
    VIXL_ASSERT(rd.IsS() || rd.IsD());
    VIXL_ASSERT(rd.GetType() == rn.GetType());
    VIXL_ASSERT(rd.GetType() == rm.GetType());
    if (rd.IsS()) {
      Vsub(cond, F32, rd.S(), rn.S(), rm.S());
    } else {
      Vsub(cond, F64, rd.D(), rn.D(), rm.D());
    }
  }
  void Vsub(VRegister rd, VRegister rn, VRegister rm) { Vsub(al, rd, rn, rm); }
  // End of generated code.

  virtual bool AllowUnpredictable() VIXL_OVERRIDE {
    VIXL_ABORT_WITH_MSG("Unpredictable instruction.\n");
    return false;
  }
  virtual bool AllowStronglyDiscouraged() VIXL_OVERRIDE {
    VIXL_ABORT_WITH_MSG(
        "ARM strongly recommends to not use this instruction.\n");
    return false;
  }
  // Old syntax of vrint instructions.
  VIXL_DEPRECATED(
      "void Vrinta(DataType dt, DRegister rd, DRegister rm)",
      void Vrinta(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrinta(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrinta(DataType dt, QRegister rd, QRegister rm)",
      void Vrinta(DataType dt1, DataType dt2, QRegister rd, QRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrinta(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrinta(DataType dt, SRegister rd, SRegister rm)",
      void Vrinta(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrinta(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintm(DataType dt, DRegister rd, DRegister rm)",
      void Vrintm(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintm(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintm(DataType dt, QRegister rd, QRegister rm)",
      void Vrintm(DataType dt1, DataType dt2, QRegister rd, QRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintm(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintm(DataType dt, SRegister rd, SRegister rm)",
      void Vrintm(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintm(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintn(DataType dt, DRegister rd, DRegister rm)",
      void Vrintn(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintn(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintn(DataType dt, QRegister rd, QRegister rm)",
      void Vrintn(DataType dt1, DataType dt2, QRegister rd, QRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintn(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintn(DataType dt, SRegister rd, SRegister rm)",
      void Vrintn(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintn(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintp(DataType dt, DRegister rd, DRegister rm)",
      void Vrintp(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintp(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintp(DataType dt, QRegister rd, QRegister rm)",
      void Vrintp(DataType dt1, DataType dt2, QRegister rd, QRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintp(dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintp(DataType dt, SRegister rd, SRegister rm)",
      void Vrintp(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintp(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintr(Condition cond, DataType dt, SRegister rd, SRegister rm)",
      void Vrintr(Condition cond,
                  DataType dt1,
                  DataType dt2,
                  SRegister rd,
                  SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintr(cond, dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintr(DataType dt, SRegister rd, SRegister rm)",
      void Vrintr(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintr(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintr(Condition cond, DataType dt, DRegister rd, DRegister rm)",
      void Vrintr(Condition cond,
                  DataType dt1,
                  DataType dt2,
                  DRegister rd,
                  DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintr(cond, dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintr(DataType dt, DRegister rd, DRegister rm)",
      void Vrintr(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintr(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintx(Condition cond, DataType dt, DRegister rd, DRegister rm)",
      void Vrintx(Condition cond,
                  DataType dt1,
                  DataType dt2,
                  DRegister rd,
                  DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintx(cond, dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintx(DataType dt, DRegister rd, DRegister rm)",
      void Vrintx(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintx(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintx(DataType dt, QRegister rd, QRegister rm)",
      void Vrintx(DataType dt1, DataType dt2, QRegister rd, QRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintx(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintx(Condition cond, DataType dt, SRegister rd, SRegister rm)",
      void Vrintx(Condition cond,
                  DataType dt1,
                  DataType dt2,
                  SRegister rd,
                  SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintx(cond, dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintx(DataType dt, SRegister rd, SRegister rm)",
      void Vrintx(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintx(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintz(Condition cond, DataType dt, DRegister rd, DRegister rm)",
      void Vrintz(Condition cond,
                  DataType dt1,
                  DataType dt2,
                  DRegister rd,
                  DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintz(cond, dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintz(DataType dt, DRegister rd, DRegister rm)",
      void Vrintz(DataType dt1, DataType dt2, DRegister rd, DRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintz(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintz(DataType dt, QRegister rd, QRegister rm)",
      void Vrintz(DataType dt1, DataType dt2, QRegister rd, QRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintz(dt1, rd, rm);
  }

  VIXL_DEPRECATED(
      "void Vrintz(Condition cond, DataType dt, SRegister rd, SRegister rm)",
      void Vrintz(Condition cond,
                  DataType dt1,
                  DataType dt2,
                  SRegister rd,
                  SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintz(cond, dt1, rd, rm);
  }
  VIXL_DEPRECATED(
      "void Vrintz(DataType dt, SRegister rd, SRegister rm)",
      void Vrintz(DataType dt1, DataType dt2, SRegister rd, SRegister rm)) {
    USE(dt2);
    VIXL_ASSERT(dt1.Is(dt2));
    return Vrintz(dt1, rd, rm);
  }

 private:
  bool NeedBranch(Condition* cond) { return !cond->Is(al) && IsUsingT32(); }
  static const int kBranchSize = kMaxInstructionSizeInBytes;

  RegisterList available_;
  VRegisterList available_vfp_;
  UseScratchRegisterScope* current_scratch_scope_;
  MacroAssemblerContext context_;
  PoolManager<int32_t> pool_manager_;
  bool generate_simulator_code_;
  bool allow_macro_instructions_;
  Label* pool_end_;

  friend class TestMacroAssembler;
};

// This scope utility allows scratch registers to be managed safely. The
// MacroAssembler's GetScratchRegisterList() is used as a pool of scratch
// registers. These registers can be allocated on demand, and will be returned
// at the end of the scope.
//
// When the scope ends, the MacroAssembler's lists will be restored to their
// original state, even if the lists were modified by some other means.
//
// Scopes must nest perfectly. That is, they must be destructed in reverse
// construction order. Otherwise, it is not clear how to handle cases where one
// scope acquires a register that was included in a now-closing scope. With
// perfect nesting, this cannot occur.
class UseScratchRegisterScope {
 public:
  // This constructor implicitly calls the `Open` function to initialise the
  // scope, so it is ready to use immediately after it has been constructed.
  explicit UseScratchRegisterScope(MacroAssembler* masm)
      : masm_(NULL), parent_(NULL), old_available_(0), old_available_vfp_(0) {
    Open(masm);
  }
  // This constructor allows deferred and optional initialisation of the scope.
  // The user is required to explicitly call the `Open` function before using
  // the scope.
  UseScratchRegisterScope()
      : masm_(NULL), parent_(NULL), old_available_(0), old_available_vfp_(0) {}

  // This function performs the actual initialisation work.
  void Open(MacroAssembler* masm);

  // The destructor always implicitly calls the `Close` function.
  ~UseScratchRegisterScope() { Close(); }

  // This function performs the cleaning-up work. It must succeed even if the
  // scope has not been opened. It is safe to call multiple times.
  void Close();

  bool IsAvailable(const Register& reg) const;
  bool IsAvailable(const VRegister& reg) const;

  // Take a register from the temp list. It will be returned automatically when
  // the scope ends.
  Register Acquire();
  VRegister AcquireV(unsigned size_in_bits);
  QRegister AcquireQ();
  DRegister AcquireD();
  SRegister AcquireS();

  // Explicitly release an acquired (or excluded) register, putting it back in
  // the temp list.
  void Release(const Register& reg);
  void Release(const VRegister& reg);

  // Make the specified registers available as scratch registers for the
  // duration of this scope.
  void Include(const RegisterList& list);
  void Include(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg) {
    Include(RegisterList(reg1, reg2, reg3, reg4));
  }
  void Include(const VRegisterList& list);
  void Include(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg) {
    Include(VRegisterList(reg1, reg2, reg3, reg4));
  }

  // Make sure that the specified registers are not available in this scope.
  // This can be used to prevent helper functions from using sensitive
  // registers, for example.
  void Exclude(const RegisterList& list);
  void Exclude(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg) {
    Exclude(RegisterList(reg1, reg2, reg3, reg4));
  }
  void Exclude(const VRegisterList& list);
  void Exclude(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg) {
    Exclude(VRegisterList(reg1, reg2, reg3, reg4));
  }

  // A convenience helper to exclude any registers used by the operand.
  void Exclude(const Operand& operand);

  // Prevent any scratch registers from being used in this scope.
  void ExcludeAll();

 private:
  // The MacroAssembler maintains a list of available scratch registers, and
  // also keeps track of the most recently-opened scope so that on destruction
  // we can check that scopes do not outlive their parents.
  MacroAssembler* masm_;
  UseScratchRegisterScope* parent_;

  // The state of the available lists at the start of this scope.
  uint32_t old_available_;      // kRRegister
  uint64_t old_available_vfp_;  // kVRegister

  VIXL_DEBUG_NO_RETURN UseScratchRegisterScope(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
  VIXL_DEBUG_NO_RETURN void operator=(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
};


}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_AARCH32_MACRO_ASSEMBLER_AARCH32_H_
