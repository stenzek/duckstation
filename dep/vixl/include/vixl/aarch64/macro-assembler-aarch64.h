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

#ifndef VIXL_AARCH64_MACRO_ASSEMBLER_AARCH64_H_
#define VIXL_AARCH64_MACRO_ASSEMBLER_AARCH64_H_

#include <algorithm>
#include <limits>

#include "../code-generation-scopes-vixl.h"
#include "../globals-vixl.h"
#include "../macro-assembler-interface.h"

#include "assembler-aarch64.h"
#include "instrument-aarch64.h"
// Required for runtime call support.
// TODO: Break this dependency. We should be able to separate out the necessary
// parts so that we don't need to include the whole simulator header.
#include "simulator-aarch64.h"
// Required in order to generate debugging instructions for the simulator. This
// is needed regardless of whether the simulator is included or not, since
// generating simulator specific instructions is controlled at runtime.
#include "simulator-constants-aarch64.h"


#define LS_MACRO_LIST(V)                                     \
  V(Ldrb, Register&, rt, LDRB_w)                             \
  V(Strb, Register&, rt, STRB_w)                             \
  V(Ldrsb, Register&, rt, rt.Is64Bits() ? LDRSB_x : LDRSB_w) \
  V(Ldrh, Register&, rt, LDRH_w)                             \
  V(Strh, Register&, rt, STRH_w)                             \
  V(Ldrsh, Register&, rt, rt.Is64Bits() ? LDRSH_x : LDRSH_w) \
  V(Ldr, CPURegister&, rt, LoadOpFor(rt))                    \
  V(Str, CPURegister&, rt, StoreOpFor(rt))                   \
  V(Ldrsw, Register&, rt, LDRSW_x)


#define LSPAIR_MACRO_LIST(V)                             \
  V(Ldp, CPURegister&, rt, rt2, LoadPairOpFor(rt, rt2))  \
  V(Stp, CPURegister&, rt, rt2, StorePairOpFor(rt, rt2)) \
  V(Ldpsw, CPURegister&, rt, rt2, LDPSW_x)

namespace vixl {
namespace aarch64 {

// Forward declaration
class MacroAssembler;
class UseScratchRegisterScope;

class Pool {
 public:
  explicit Pool(MacroAssembler* masm)
      : checkpoint_(kNoCheckpointRequired), masm_(masm) {
    Reset();
  }

  void Reset() {
    checkpoint_ = kNoCheckpointRequired;
    monitor_ = 0;
  }

  void Block() { monitor_++; }
  void Release();
  bool IsBlocked() const { return monitor_ != 0; }

  static const ptrdiff_t kNoCheckpointRequired = PTRDIFF_MAX;

  void SetNextCheckpoint(ptrdiff_t checkpoint);
  ptrdiff_t GetCheckpoint() const { return checkpoint_; }
  VIXL_DEPRECATED("GetCheckpoint", ptrdiff_t checkpoint() const) {
    return GetCheckpoint();
  }

  enum EmitOption { kBranchRequired, kNoBranchRequired };

 protected:
  // Next buffer offset at which a check is required for this pool.
  ptrdiff_t checkpoint_;
  // Indicates whether the emission of this pool is blocked.
  int monitor_;
  // The MacroAssembler using this pool.
  MacroAssembler* masm_;
};


class LiteralPool : public Pool {
 public:
  explicit LiteralPool(MacroAssembler* masm);
  ~LiteralPool();
  void Reset();

  void AddEntry(RawLiteral* literal);
  bool IsEmpty() const { return entries_.empty(); }
  size_t GetSize() const;
  VIXL_DEPRECATED("GetSize", size_t Size() const) { return GetSize(); }

  size_t GetMaxSize() const;
  VIXL_DEPRECATED("GetMaxSize", size_t MaxSize() const) { return GetMaxSize(); }

  size_t GetOtherPoolsMaxSize() const;
  VIXL_DEPRECATED("GetOtherPoolsMaxSize", size_t OtherPoolsMaxSize() const) {
    return GetOtherPoolsMaxSize();
  }

  void CheckEmitFor(size_t amount, EmitOption option = kBranchRequired);
  // Check whether we need to emit the literal pool in order to be able to
  // safely emit a branch with a given range.
  void CheckEmitForBranch(size_t range);
  void Emit(EmitOption option = kNoBranchRequired);

  void SetNextRecommendedCheckpoint(ptrdiff_t offset);
  ptrdiff_t GetNextRecommendedCheckpoint();
  VIXL_DEPRECATED("GetNextRecommendedCheckpoint",
                  ptrdiff_t NextRecommendedCheckpoint()) {
    return GetNextRecommendedCheckpoint();
  }

  void UpdateFirstUse(ptrdiff_t use_position);

  void DeleteOnDestruction(RawLiteral* literal) {
    deleted_on_destruction_.push_back(literal);
  }

  // Recommended not exact since the pool can be blocked for short periods.
  static const ptrdiff_t kRecommendedLiteralPoolRange = 128 * KBytes;

 private:
  std::vector<RawLiteral*> entries_;
  size_t size_;
  ptrdiff_t first_use_;
  // The parent class `Pool` provides a `checkpoint_`, which is the buffer
  // offset before which a check *must* occur. This recommended checkpoint
  // indicates when we would like to start emitting the constant pool. The
  // MacroAssembler can, but does not have to, check the buffer when the
  // checkpoint is reached.
  ptrdiff_t recommended_checkpoint_;

  std::vector<RawLiteral*> deleted_on_destruction_;
};


inline size_t LiteralPool::GetSize() const {
  // Account for the pool header.
  return size_ + kInstructionSize;
}


inline size_t LiteralPool::GetMaxSize() const {
  // Account for the potential branch over the pool.
  return GetSize() + kInstructionSize;
}


inline ptrdiff_t LiteralPool::GetNextRecommendedCheckpoint() {
  return first_use_ + kRecommendedLiteralPoolRange;
}


class VeneerPool : public Pool {
 public:
  explicit VeneerPool(MacroAssembler* masm) : Pool(masm) {}

  void Reset();

  void Block() { monitor_++; }
  void Release();
  bool IsBlocked() const { return monitor_ != 0; }
  bool IsEmpty() const { return unresolved_branches_.IsEmpty(); }

  class BranchInfo {
   public:
    BranchInfo()
        : first_unreacheable_pc_(0),
          pc_offset_(0),
          label_(NULL),
          branch_type_(UnknownBranchType) {}
    BranchInfo(ptrdiff_t offset, Label* label, ImmBranchType branch_type)
        : pc_offset_(offset), label_(label), branch_type_(branch_type) {
      first_unreacheable_pc_ =
          pc_offset_ + Instruction::GetImmBranchForwardRange(branch_type_);
    }

    static bool IsValidComparison(const BranchInfo& branch_1,
                                  const BranchInfo& branch_2) {
      // BranchInfo are always compared against against other objects with
      // the same branch type.
      if (branch_1.branch_type_ != branch_2.branch_type_) {
        return false;
      }
      // Since we should never have two branch infos with the same offsets, it
      // first looks like we should check that offsets are different. However
      // the operators may also be used to *search* for a branch info in the
      // set.
      bool same_offsets = (branch_1.pc_offset_ == branch_2.pc_offset_);
      return (!same_offsets || ((branch_1.label_ == branch_2.label_) &&
                                (branch_1.first_unreacheable_pc_ ==
                                 branch_2.first_unreacheable_pc_)));
    }

    // We must provide comparison operators to work with InvalSet.
    bool operator==(const BranchInfo& other) const {
      VIXL_ASSERT(IsValidComparison(*this, other));
      return pc_offset_ == other.pc_offset_;
    }
    bool operator<(const BranchInfo& other) const {
      VIXL_ASSERT(IsValidComparison(*this, other));
      return pc_offset_ < other.pc_offset_;
    }
    bool operator<=(const BranchInfo& other) const {
      VIXL_ASSERT(IsValidComparison(*this, other));
      return pc_offset_ <= other.pc_offset_;
    }
    bool operator>(const BranchInfo& other) const {
      VIXL_ASSERT(IsValidComparison(*this, other));
      return pc_offset_ > other.pc_offset_;
    }

    // First instruction position that is not reachable by the branch using a
    // positive branch offset.
    ptrdiff_t first_unreacheable_pc_;
    // Offset of the branch in the code generation buffer.
    ptrdiff_t pc_offset_;
    // The label branched to.
    Label* label_;
    ImmBranchType branch_type_;
  };

  bool BranchTypeUsesVeneers(ImmBranchType type) {
    return (type != UnknownBranchType) && (type != UncondBranchType);
  }

  void RegisterUnresolvedBranch(ptrdiff_t branch_pos,
                                Label* label,
                                ImmBranchType branch_type);
  void DeleteUnresolvedBranchInfoForLabel(Label* label);

  bool ShouldEmitVeneer(int64_t first_unreacheable_pc, size_t amount);
  bool ShouldEmitVeneers(size_t amount) {
    return ShouldEmitVeneer(unresolved_branches_.GetFirstLimit(), amount);
  }

  void CheckEmitFor(size_t amount, EmitOption option = kBranchRequired);
  void Emit(EmitOption option, size_t margin);

  // The code size generated for a veneer. Currently one branch instruction.
  // This is for code size checking purposes, and can be extended in the future
  // for example if we decide to add nops between the veneers.
  static const int kVeneerCodeSize = 1 * kInstructionSize;
  // The maximum size of code other than veneers that can be generated when
  // emitting a veneer pool. Currently there can be an additional branch to jump
  // over the pool.
  static const int kPoolNonVeneerCodeSize = 1 * kInstructionSize;

  void UpdateNextCheckPoint() { SetNextCheckpoint(GetNextCheckPoint()); }

  int GetNumberOfPotentialVeneers() const {
    return static_cast<int>(unresolved_branches_.GetSize());
  }
  VIXL_DEPRECATED("GetNumberOfPotentialVeneers",
                  int NumberOfPotentialVeneers() const) {
    return GetNumberOfPotentialVeneers();
  }

  size_t GetMaxSize() const {
    return kPoolNonVeneerCodeSize +
           unresolved_branches_.GetSize() * kVeneerCodeSize;
  }
  VIXL_DEPRECATED("GetMaxSize", size_t MaxSize() const) { return GetMaxSize(); }

  size_t GetOtherPoolsMaxSize() const;
  VIXL_DEPRECATED("GetOtherPoolsMaxSize", size_t OtherPoolsMaxSize() const) {
    return GetOtherPoolsMaxSize();
  }

  static const int kNPreallocatedInfos = 4;
  static const ptrdiff_t kInvalidOffset = PTRDIFF_MAX;
  static const size_t kReclaimFrom = 128;
  static const size_t kReclaimFactor = 16;

 private:
  typedef InvalSet<BranchInfo,
                   kNPreallocatedInfos,
                   ptrdiff_t,
                   kInvalidOffset,
                   kReclaimFrom,
                   kReclaimFactor>
      BranchInfoTypedSetBase;
  typedef InvalSetIterator<BranchInfoTypedSetBase> BranchInfoTypedSetIterBase;

  class BranchInfoTypedSet : public BranchInfoTypedSetBase {
   public:
    BranchInfoTypedSet() : BranchInfoTypedSetBase() {}

    ptrdiff_t GetFirstLimit() {
      if (empty()) {
        return kInvalidOffset;
      }
      return GetMinElementKey();
    }
    VIXL_DEPRECATED("GetFirstLimit", ptrdiff_t FirstLimit()) {
      return GetFirstLimit();
    }
  };

  class BranchInfoTypedSetIterator : public BranchInfoTypedSetIterBase {
   public:
    BranchInfoTypedSetIterator() : BranchInfoTypedSetIterBase(NULL) {}
    explicit BranchInfoTypedSetIterator(BranchInfoTypedSet* typed_set)
        : BranchInfoTypedSetIterBase(typed_set) {}

    // TODO: Remove these and use the STL-like interface instead.
    using BranchInfoTypedSetIterBase::Advance;
    using BranchInfoTypedSetIterBase::Current;
  };

  class BranchInfoSet {
   public:
    void insert(BranchInfo branch_info) {
      ImmBranchType type = branch_info.branch_type_;
      VIXL_ASSERT(IsValidBranchType(type));
      typed_set_[BranchIndexFromType(type)].insert(branch_info);
    }

    void erase(BranchInfo branch_info) {
      if (IsValidBranchType(branch_info.branch_type_)) {
        int index =
            BranchInfoSet::BranchIndexFromType(branch_info.branch_type_);
        typed_set_[index].erase(branch_info);
      }
    }

    size_t GetSize() const {
      size_t res = 0;
      for (int i = 0; i < kNumberOfTrackedBranchTypes; i++) {
        res += typed_set_[i].size();
      }
      return res;
    }
    VIXL_DEPRECATED("GetSize", size_t size() const) { return GetSize(); }

    bool IsEmpty() const {
      for (int i = 0; i < kNumberOfTrackedBranchTypes; i++) {
        if (!typed_set_[i].empty()) {
          return false;
        }
      }
      return true;
    }
    VIXL_DEPRECATED("IsEmpty", bool empty() const) { return IsEmpty(); }

    ptrdiff_t GetFirstLimit() {
      ptrdiff_t res = kInvalidOffset;
      for (int i = 0; i < kNumberOfTrackedBranchTypes; i++) {
        res = std::min(res, typed_set_[i].GetFirstLimit());
      }
      return res;
    }
    VIXL_DEPRECATED("GetFirstLimit", ptrdiff_t FirstLimit()) {
      return GetFirstLimit();
    }

    void Reset() {
      for (int i = 0; i < kNumberOfTrackedBranchTypes; i++) {
        typed_set_[i].clear();
      }
    }

    static ImmBranchType BranchTypeFromIndex(int index) {
      switch (index) {
        case 0:
          return CondBranchType;
        case 1:
          return CompareBranchType;
        case 2:
          return TestBranchType;
        default:
          VIXL_UNREACHABLE();
          return UnknownBranchType;
      }
    }
    static int BranchIndexFromType(ImmBranchType branch_type) {
      switch (branch_type) {
        case CondBranchType:
          return 0;
        case CompareBranchType:
          return 1;
        case TestBranchType:
          return 2;
        default:
          VIXL_UNREACHABLE();
          return 0;
      }
    }

    bool IsValidBranchType(ImmBranchType branch_type) {
      return (branch_type != UnknownBranchType) &&
             (branch_type != UncondBranchType);
    }

   private:
    static const int kNumberOfTrackedBranchTypes = 3;
    BranchInfoTypedSet typed_set_[kNumberOfTrackedBranchTypes];

    friend class VeneerPool;
    friend class BranchInfoSetIterator;
  };

  class BranchInfoSetIterator {
   public:
    explicit BranchInfoSetIterator(BranchInfoSet* set) : set_(set) {
      for (int i = 0; i < BranchInfoSet::kNumberOfTrackedBranchTypes; i++) {
        new (&sub_iterator_[i])
            BranchInfoTypedSetIterator(&(set_->typed_set_[i]));
      }
    }

    VeneerPool::BranchInfo* Current() {
      for (int i = 0; i < BranchInfoSet::kNumberOfTrackedBranchTypes; i++) {
        if (!sub_iterator_[i].Done()) {
          return sub_iterator_[i].Current();
        }
      }
      VIXL_UNREACHABLE();
      return NULL;
    }

    void Advance() {
      VIXL_ASSERT(!Done());
      for (int i = 0; i < BranchInfoSet::kNumberOfTrackedBranchTypes; i++) {
        if (!sub_iterator_[i].Done()) {
          sub_iterator_[i].Advance();
          return;
        }
      }
      VIXL_UNREACHABLE();
    }

    bool Done() const {
      for (int i = 0; i < BranchInfoSet::kNumberOfTrackedBranchTypes; i++) {
        if (!sub_iterator_[i].Done()) return false;
      }
      return true;
    }

    void AdvanceToNextType() {
      VIXL_ASSERT(!Done());
      for (int i = 0; i < BranchInfoSet::kNumberOfTrackedBranchTypes; i++) {
        if (!sub_iterator_[i].Done()) {
          sub_iterator_[i].Finish();
          return;
        }
      }
      VIXL_UNREACHABLE();
    }

    void DeleteCurrentAndAdvance() {
      for (int i = 0; i < BranchInfoSet::kNumberOfTrackedBranchTypes; i++) {
        if (!sub_iterator_[i].Done()) {
          sub_iterator_[i].DeleteCurrentAndAdvance();
          return;
        }
      }
    }

   private:
    BranchInfoSet* set_;
    BranchInfoTypedSetIterator
        sub_iterator_[BranchInfoSet::kNumberOfTrackedBranchTypes];
  };

  ptrdiff_t GetNextCheckPoint() {
    if (unresolved_branches_.IsEmpty()) {
      return kNoCheckpointRequired;
    } else {
      return unresolved_branches_.GetFirstLimit();
    }
  }
  VIXL_DEPRECATED("GetNextCheckPoint", ptrdiff_t NextCheckPoint()) {
    return GetNextCheckPoint();
  }

  // Information about unresolved (forward) branches.
  BranchInfoSet unresolved_branches_;
};


// Helper for common Emission checks.
// The macro-instruction maps to a single instruction.
class SingleEmissionCheckScope : public EmissionCheckScope {
 public:
  explicit SingleEmissionCheckScope(MacroAssemblerInterface* masm)
      : EmissionCheckScope(masm, kInstructionSize) {}
};


// The macro instruction is a "typical" macro-instruction. Typical macro-
// instruction only emit a few instructions, a few being defined as 8 here.
class MacroEmissionCheckScope : public EmissionCheckScope {
 public:
  explicit MacroEmissionCheckScope(MacroAssemblerInterface* masm)
      : EmissionCheckScope(masm, kTypicalMacroInstructionMaxSize) {}

 private:
  static const size_t kTypicalMacroInstructionMaxSize = 8 * kInstructionSize;
};


enum BranchType {
  // Copies of architectural conditions.
  // The associated conditions can be used in place of those, the code will
  // take care of reinterpreting them with the correct type.
  integer_eq = eq,
  integer_ne = ne,
  integer_hs = hs,
  integer_lo = lo,
  integer_mi = mi,
  integer_pl = pl,
  integer_vs = vs,
  integer_vc = vc,
  integer_hi = hi,
  integer_ls = ls,
  integer_ge = ge,
  integer_lt = lt,
  integer_gt = gt,
  integer_le = le,
  integer_al = al,
  integer_nv = nv,

  // These two are *different* from the architectural codes al and nv.
  // 'always' is used to generate unconditional branches.
  // 'never' is used to not generate a branch (generally as the inverse
  // branch type of 'always).
  always,
  never,
  // cbz and cbnz
  reg_zero,
  reg_not_zero,
  // tbz and tbnz
  reg_bit_clear,
  reg_bit_set,

  // Aliases.
  kBranchTypeFirstCondition = eq,
  kBranchTypeLastCondition = nv,
  kBranchTypeFirstUsingReg = reg_zero,
  kBranchTypeFirstUsingBit = reg_bit_clear
};


enum DiscardMoveMode { kDontDiscardForSameWReg, kDiscardForSameWReg };

// The macro assembler supports moving automatically pre-shifted immediates for
// arithmetic and logical instructions, and then applying a post shift in the
// instruction to undo the modification, in order to reduce the code emitted for
// an operation. For example:
//
//  Add(x0, x0, 0x1f7de) => movz x16, 0xfbef; add x0, x0, x16, lsl #1.
//
// This optimisation can be only partially applied when the stack pointer is an
// operand or destination, so this enumeration is used to control the shift.
enum PreShiftImmMode {
  kNoShift,          // Don't pre-shift.
  kLimitShiftForSP,  // Limit pre-shift for add/sub extend use.
  kAnyShift          // Allow any pre-shift.
};


class MacroAssembler : public Assembler, public MacroAssemblerInterface {
 public:
  explicit MacroAssembler(
      PositionIndependentCodeOption pic = PositionIndependentCode);
  MacroAssembler(size_t capacity,
                 PositionIndependentCodeOption pic = PositionIndependentCode);
  MacroAssembler(byte* buffer,
                 size_t capacity,
                 PositionIndependentCodeOption pic = PositionIndependentCode);
  ~MacroAssembler();

  enum FinalizeOption {
    kFallThrough,  // There may be more code to execute after calling Finalize.
    kUnreachable   // Anything generated after calling Finalize is unreachable.
  };

  virtual vixl::internal::AssemblerBase* AsAssemblerBase() VIXL_OVERRIDE {
    return this;
  }

  // TODO(pools): implement these functions.
  virtual void EmitPoolHeader() VIXL_OVERRIDE {}
  virtual void EmitPoolFooter() VIXL_OVERRIDE {}
  virtual void EmitPaddingBytes(int n) VIXL_OVERRIDE { USE(n); }
  virtual void EmitNopBytes(int n) VIXL_OVERRIDE { USE(n); }

  // Start generating code from the beginning of the buffer, discarding any code
  // and data that has already been emitted into the buffer.
  //
  // In order to avoid any accidental transfer of state, Reset ASSERTs that the
  // constant pool is not blocked.
  void Reset();

  // Finalize a code buffer of generated instructions. This function must be
  // called before executing or copying code from the buffer. By default,
  // anything generated after this should not be reachable (the last instruction
  // generated is an unconditional branch). If you need to generate more code,
  // then set `option` to kFallThrough.
  void FinalizeCode(FinalizeOption option = kUnreachable);


  // Constant generation helpers.
  // These functions return the number of instructions required to move the
  // immediate into the destination register. Also, if the masm pointer is
  // non-null, it generates the code to do so.
  // The two features are implemented using one function to avoid duplication of
  // the logic.
  // The function can be used to evaluate the cost of synthesizing an
  // instruction using 'mov immediate' instructions. A user might prefer loading
  // a constant using the literal pool instead of using multiple 'mov immediate'
  // instructions.
  static int MoveImmediateHelper(MacroAssembler* masm,
                                 const Register& rd,
                                 uint64_t imm);
  static bool OneInstrMoveImmediateHelper(MacroAssembler* masm,
                                          const Register& dst,
                                          int64_t imm);


  // Logical macros.
  void And(const Register& rd, const Register& rn, const Operand& operand);
  void Ands(const Register& rd, const Register& rn, const Operand& operand);
  void Bic(const Register& rd, const Register& rn, const Operand& operand);
  void Bics(const Register& rd, const Register& rn, const Operand& operand);
  void Orr(const Register& rd, const Register& rn, const Operand& operand);
  void Orn(const Register& rd, const Register& rn, const Operand& operand);
  void Eor(const Register& rd, const Register& rn, const Operand& operand);
  void Eon(const Register& rd, const Register& rn, const Operand& operand);
  void Tst(const Register& rn, const Operand& operand);
  void LogicalMacro(const Register& rd,
                    const Register& rn,
                    const Operand& operand,
                    LogicalOp op);

  // Add and sub macros.
  void Add(const Register& rd,
           const Register& rn,
           const Operand& operand,
           FlagsUpdate S = LeaveFlags);
  void Adds(const Register& rd, const Register& rn, const Operand& operand);
  void Sub(const Register& rd,
           const Register& rn,
           const Operand& operand,
           FlagsUpdate S = LeaveFlags);
  void Subs(const Register& rd, const Register& rn, const Operand& operand);
  void Cmn(const Register& rn, const Operand& operand);
  void Cmp(const Register& rn, const Operand& operand);
  void Neg(const Register& rd, const Operand& operand);
  void Negs(const Register& rd, const Operand& operand);

  void AddSubMacro(const Register& rd,
                   const Register& rn,
                   const Operand& operand,
                   FlagsUpdate S,
                   AddSubOp op);

  // Add/sub with carry macros.
  void Adc(const Register& rd, const Register& rn, const Operand& operand);
  void Adcs(const Register& rd, const Register& rn, const Operand& operand);
  void Sbc(const Register& rd, const Register& rn, const Operand& operand);
  void Sbcs(const Register& rd, const Register& rn, const Operand& operand);
  void Ngc(const Register& rd, const Operand& operand);
  void Ngcs(const Register& rd, const Operand& operand);
  void AddSubWithCarryMacro(const Register& rd,
                            const Register& rn,
                            const Operand& operand,
                            FlagsUpdate S,
                            AddSubWithCarryOp op);

  // Move macros.
  void Mov(const Register& rd, uint64_t imm);
  void Mov(const Register& rd,
           const Operand& operand,
           DiscardMoveMode discard_mode = kDontDiscardForSameWReg);
  void Mvn(const Register& rd, uint64_t imm) {
    Mov(rd, (rd.GetSizeInBits() == kXRegSize) ? ~imm : (~imm & kWRegMask));
  }
  void Mvn(const Register& rd, const Operand& operand);

  // Try to move an immediate into the destination register in a single
  // instruction. Returns true for success, and updates the contents of dst.
  // Returns false, otherwise.
  bool TryOneInstrMoveImmediate(const Register& dst, int64_t imm);

  // Move an immediate into register dst, and return an Operand object for
  // use with a subsequent instruction that accepts a shift. The value moved
  // into dst is not necessarily equal to imm; it may have had a shifting
  // operation applied to it that will be subsequently undone by the shift
  // applied in the Operand.
  Operand MoveImmediateForShiftedOp(const Register& dst,
                                    int64_t imm,
                                    PreShiftImmMode mode);

  void Move(const GenericOperand& dst, const GenericOperand& src);

  // Synthesises the address represented by a MemOperand into a register.
  void ComputeAddress(const Register& dst, const MemOperand& mem_op);

  // Conditional macros.
  void Ccmp(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);
  void Ccmn(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);
  void ConditionalCompareMacro(const Register& rn,
                               const Operand& operand,
                               StatusFlags nzcv,
                               Condition cond,
                               ConditionalCompareOp op);

  // On return, the boolean values pointed to will indicate whether `left` and
  // `right` should be synthesised in a temporary register.
  static void GetCselSynthesisInformation(const Register& rd,
                                          const Operand& left,
                                          const Operand& right,
                                          bool* should_synthesise_left,
                                          bool* should_synthesise_right) {
    // Note that the helper does not need to look at the condition.
    CselHelper(NULL,
               rd,
               left,
               right,
               eq,
               should_synthesise_left,
               should_synthesise_right);
  }

  void Csel(const Register& rd,
            const Operand& left,
            const Operand& right,
            Condition cond) {
    CselHelper(this, rd, left, right, cond);
  }

// Load/store macros.
#define DECLARE_FUNCTION(FN, REGTYPE, REG, OP) \
  void FN(const REGTYPE REG, const MemOperand& addr);
  LS_MACRO_LIST(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

  void LoadStoreMacro(const CPURegister& rt,
                      const MemOperand& addr,
                      LoadStoreOp op);

#define DECLARE_FUNCTION(FN, REGTYPE, REG, REG2, OP) \
  void FN(const REGTYPE REG, const REGTYPE REG2, const MemOperand& addr);
  LSPAIR_MACRO_LIST(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

  void LoadStorePairMacro(const CPURegister& rt,
                          const CPURegister& rt2,
                          const MemOperand& addr,
                          LoadStorePairOp op);

  void Prfm(PrefetchOperation op, const MemOperand& addr);

  // Push or pop up to 4 registers of the same width to or from the stack,
  // using the current stack pointer as set by SetStackPointer.
  //
  // If an argument register is 'NoReg', all further arguments are also assumed
  // to be 'NoReg', and are thus not pushed or popped.
  //
  // Arguments are ordered such that "Push(a, b);" is functionally equivalent
  // to "Push(a); Push(b);".
  //
  // It is valid to push the same register more than once, and there is no
  // restriction on the order in which registers are specified.
  //
  // It is not valid to pop into the same register more than once in one
  // operation, not even into the zero register.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then it
  // must be aligned to 16 bytes on entry and the total size of the specified
  // registers must also be a multiple of 16 bytes.
  //
  // Even if the current stack pointer is not the system stack pointer (sp),
  // Push (and derived methods) will still modify the system stack pointer in
  // order to comply with ABI rules about accessing memory below the system
  // stack pointer.
  //
  // Other than the registers passed into Pop, the stack pointer and (possibly)
  // the system stack pointer, these methods do not modify any other registers.
  void Push(const CPURegister& src0,
            const CPURegister& src1 = NoReg,
            const CPURegister& src2 = NoReg,
            const CPURegister& src3 = NoReg);
  void Pop(const CPURegister& dst0,
           const CPURegister& dst1 = NoReg,
           const CPURegister& dst2 = NoReg,
           const CPURegister& dst3 = NoReg);

  // Alternative forms of Push and Pop, taking a RegList or CPURegList that
  // specifies the registers that are to be pushed or popped. Higher-numbered
  // registers are associated with higher memory addresses (as in the A32 push
  // and pop instructions).
  //
  // (Push|Pop)SizeRegList allow you to specify the register size as a
  // parameter. Only kXRegSize, kWRegSize, kDRegSize and kSRegSize are
  // supported.
  //
  // Otherwise, (Push|Pop)(CPU|X|W|D|S)RegList is preferred.
  void PushCPURegList(CPURegList registers);
  void PopCPURegList(CPURegList registers);

  void PushSizeRegList(
      RegList registers,
      unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PushCPURegList(CPURegList(type, reg_size, registers));
  }
  void PopSizeRegList(RegList registers,
                      unsigned reg_size,
                      CPURegister::RegisterType type = CPURegister::kRegister) {
    PopCPURegList(CPURegList(type, reg_size, registers));
  }
  void PushXRegList(RegList regs) { PushSizeRegList(regs, kXRegSize); }
  void PopXRegList(RegList regs) { PopSizeRegList(regs, kXRegSize); }
  void PushWRegList(RegList regs) { PushSizeRegList(regs, kWRegSize); }
  void PopWRegList(RegList regs) { PopSizeRegList(regs, kWRegSize); }
  void PushDRegList(RegList regs) {
    PushSizeRegList(regs, kDRegSize, CPURegister::kVRegister);
  }
  void PopDRegList(RegList regs) {
    PopSizeRegList(regs, kDRegSize, CPURegister::kVRegister);
  }
  void PushSRegList(RegList regs) {
    PushSizeRegList(regs, kSRegSize, CPURegister::kVRegister);
  }
  void PopSRegList(RegList regs) {
    PopSizeRegList(regs, kSRegSize, CPURegister::kVRegister);
  }

  // Push the specified register 'count' times.
  void PushMultipleTimes(int count, Register src);

  // Poke 'src' onto the stack. The offset is in bytes.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then sp
  // must be aligned to 16 bytes.
  void Poke(const Register& src, const Operand& offset);

  // Peek at a value on the stack, and put it in 'dst'. The offset is in bytes.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then sp
  // must be aligned to 16 bytes.
  void Peek(const Register& dst, const Operand& offset);

  // Alternative forms of Peek and Poke, taking a RegList or CPURegList that
  // specifies the registers that are to be pushed or popped. Higher-numbered
  // registers are associated with higher memory addresses.
  //
  // (Peek|Poke)SizeRegList allow you to specify the register size as a
  // parameter. Only kXRegSize, kWRegSize, kDRegSize and kSRegSize are
  // supported.
  //
  // Otherwise, (Peek|Poke)(CPU|X|W|D|S)RegList is preferred.
  void PeekCPURegList(CPURegList registers, int64_t offset) {
    LoadCPURegList(registers, MemOperand(StackPointer(), offset));
  }
  void PokeCPURegList(CPURegList registers, int64_t offset) {
    StoreCPURegList(registers, MemOperand(StackPointer(), offset));
  }

  void PeekSizeRegList(
      RegList registers,
      int64_t offset,
      unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PeekCPURegList(CPURegList(type, reg_size, registers), offset);
  }
  void PokeSizeRegList(
      RegList registers,
      int64_t offset,
      unsigned reg_size,
      CPURegister::RegisterType type = CPURegister::kRegister) {
    PokeCPURegList(CPURegList(type, reg_size, registers), offset);
  }
  void PeekXRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kXRegSize);
  }
  void PokeXRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kXRegSize);
  }
  void PeekWRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kWRegSize);
  }
  void PokeWRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kWRegSize);
  }
  void PeekDRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kDRegSize, CPURegister::kVRegister);
  }
  void PokeDRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kDRegSize, CPURegister::kVRegister);
  }
  void PeekSRegList(RegList regs, int64_t offset) {
    PeekSizeRegList(regs, offset, kSRegSize, CPURegister::kVRegister);
  }
  void PokeSRegList(RegList regs, int64_t offset) {
    PokeSizeRegList(regs, offset, kSRegSize, CPURegister::kVRegister);
  }


  // Claim or drop stack space without actually accessing memory.
  //
  // If the current stack pointer (as set by SetStackPointer) is sp, then it
  // must be aligned to 16 bytes and the size claimed or dropped must be a
  // multiple of 16 bytes.
  void Claim(const Operand& size);
  void Drop(const Operand& size);

  // Preserve the callee-saved registers (as defined by AAPCS64).
  //
  // Higher-numbered registers are pushed before lower-numbered registers, and
  // thus get higher addresses.
  // Floating-point registers are pushed before general-purpose registers, and
  // thus get higher addresses.
  //
  // This method must not be called unless StackPointer() is sp, and it is
  // aligned to 16 bytes.
  void PushCalleeSavedRegisters();

  // Restore the callee-saved registers (as defined by AAPCS64).
  //
  // Higher-numbered registers are popped after lower-numbered registers, and
  // thus come from higher addresses.
  // Floating-point registers are popped after general-purpose registers, and
  // thus come from higher addresses.
  //
  // This method must not be called unless StackPointer() is sp, and it is
  // aligned to 16 bytes.
  void PopCalleeSavedRegisters();

  void LoadCPURegList(CPURegList registers, const MemOperand& src);
  void StoreCPURegList(CPURegList registers, const MemOperand& dst);

  // Remaining instructions are simple pass-through calls to the assembler.
  void Adr(const Register& rd, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    adr(rd, label);
  }
  void Adrp(const Register& rd, Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    adrp(rd, label);
  }
  void Asr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    asr(rd, rn, shift);
  }
  void Asr(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    asrv(rd, rn, rm);
  }

  // Branch type inversion relies on these relations.
  VIXL_STATIC_ASSERT((reg_zero == (reg_not_zero ^ 1)) &&
                     (reg_bit_clear == (reg_bit_set ^ 1)) &&
                     (always == (never ^ 1)));

  BranchType InvertBranchType(BranchType type) {
    if (kBranchTypeFirstCondition <= type && type <= kBranchTypeLastCondition) {
      return static_cast<BranchType>(
          InvertCondition(static_cast<Condition>(type)));
    } else {
      return static_cast<BranchType>(type ^ 1);
    }
  }

  void B(Label* label, BranchType type, Register reg = NoReg, int bit = -1);

  void B(Label* label);
  void B(Label* label, Condition cond);
  void B(Condition cond, Label* label) { B(label, cond); }
  void Bfm(const Register& rd,
           const Register& rn,
           unsigned immr,
           unsigned imms) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    bfm(rd, rn, immr, imms);
  }
  void Bfi(const Register& rd,
           const Register& rn,
           unsigned lsb,
           unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    bfi(rd, rn, lsb, width);
  }
  void Bfc(const Register& rd, unsigned lsb, unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    bfc(rd, lsb, width);
  }
  void Bfxil(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    bfxil(rd, rn, lsb, width);
  }
  void Bind(Label* label);
  // Bind a label to a specified offset from the start of the buffer.
  void BindToOffset(Label* label, ptrdiff_t offset);
  void Bl(Label* label) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    bl(label);
  }
  void Blr(const Register& xn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!xn.IsZero());
    SingleEmissionCheckScope guard(this);
    blr(xn);
  }
  void Br(const Register& xn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!xn.IsZero());
    SingleEmissionCheckScope guard(this);
    br(xn);
  }
  void Braaz(const Register& xn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    braaz(xn);
  }
  void Brabz(const Register& xn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    brabz(xn);
  }
  void Blraaz(const Register& xn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    blraaz(xn);
  }
  void Blrabz(const Register& xn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    blrabz(xn);
  }
  void Retaa() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    retaa();
  }
  void Retab() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    retab();
  }
  void Braa(const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    braa(xn, xm);
  }
  void Brab(const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    brab(xn, xm);
  }
  void Blraa(const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    blraa(xn, xm);
  }
  void Blrab(const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    blrab(xn, xm);
  }
  void Brk(int code = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    brk(code);
  }
  void Cbnz(const Register& rt, Label* label);
  void Cbz(const Register& rt, Label* label);
  void Cinc(const Register& rd, const Register& rn, Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cinc(rd, rn, cond);
  }
  void Cinv(const Register& rd, const Register& rn, Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cinv(rd, rn, cond);
  }

#define PAUTH_SYSTEM_MODES(V) \
  V(az)                       \
  V(bz)                       \
  V(asp)                      \
  V(bsp)

#define DEFINE_MACRO_ASM_FUNCS(SUFFIX)      \
  void Paci##SUFFIX() {                     \
    VIXL_ASSERT(allow_macro_instructions_); \
    SingleEmissionCheckScope guard(this);   \
    paci##SUFFIX();                         \
  }                                         \
  void Auti##SUFFIX() {                     \
    VIXL_ASSERT(allow_macro_instructions_); \
    SingleEmissionCheckScope guard(this);   \
    auti##SUFFIX();                         \
  }

  PAUTH_SYSTEM_MODES(DEFINE_MACRO_ASM_FUNCS)
#undef DEFINE_MACRO_ASM_FUNCS

  // The 1716 pac and aut instructions encourage people to use x16 and x17
  // directly, perhaps without realising that this is forbidden. For example:
  //
  //     UseScratchRegisterScope temps(&masm);
  //     Register temp = temps.AcquireX();  // temp will be x16
  //     __ Mov(x17, ptr);
  //     __ Mov(x16, modifier);  // Will override temp!
  //     __ Pacia1716();
  //
  // To work around this issue, you must exclude x16 and x17 from the scratch
  // register list. You may need to replace them with other registers:
  //
  //     UseScratchRegisterScope temps(&masm);
  //     temps.Exclude(x16, x17);
  //     temps.Include(x10, x11);
  //     __ Mov(x17, ptr);
  //     __ Mov(x16, modifier);
  //     __ Pacia1716();
  void Pacia1716() {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x16));
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x17));
    SingleEmissionCheckScope guard(this);
    pacia1716();
  }
  void Pacib1716() {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x16));
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x17));
    SingleEmissionCheckScope guard(this);
    pacib1716();
  }
  void Autia1716() {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x16));
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x17));
    SingleEmissionCheckScope guard(this);
    autia1716();
  }
  void Autib1716() {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x16));
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(x17));
    SingleEmissionCheckScope guard(this);
    autib1716();
  }
  void Xpaclri() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    xpaclri();
  }
  void Clrex() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    clrex();
  }
  void Cls(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cls(rd, rn);
  }
  void Clz(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    clz(rd, rn);
  }
  void Cneg(const Register& rd, const Register& rn, Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    cneg(rd, rn, cond);
  }
  void Esb() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    esb();
  }
  void Csdb() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    csdb();
  }
  void Cset(const Register& rd, Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    cset(rd, cond);
  }
  void Csetm(const Register& rd, Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    csetm(rd, cond);
  }
  void Csinc(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    csinc(rd, rn, rm, cond);
  }
  void Csinv(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    csinv(rd, rn, rm, cond);
  }
  void Csneg(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    csneg(rd, rn, rm, cond);
  }
  void Dmb(BarrierDomain domain, BarrierType type) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    dmb(domain, type);
  }
  void Dsb(BarrierDomain domain, BarrierType type) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    dsb(domain, type);
  }
  void Extr(const Register& rd,
            const Register& rn,
            const Register& rm,
            unsigned lsb) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    extr(rd, rn, rm, lsb);
  }
  void Fadd(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fadd(vd, vn, vm);
  }
  void Fccmp(const VRegister& vn,
             const VRegister& vm,
             StatusFlags nzcv,
             Condition cond,
             FPTrapFlags trap = DisableTrap) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    FPCCompareMacro(vn, vm, nzcv, cond, trap);
  }
  void Fccmpe(const VRegister& vn,
              const VRegister& vm,
              StatusFlags nzcv,
              Condition cond) {
    Fccmp(vn, vm, nzcv, cond, EnableTrap);
  }
  void Fcmp(const VRegister& vn,
            const VRegister& vm,
            FPTrapFlags trap = DisableTrap) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    FPCompareMacro(vn, vm, trap);
  }
  void Fcmp(const VRegister& vn, double value, FPTrapFlags trap = DisableTrap);
  void Fcmpe(const VRegister& vn, double value);
  void Fcmpe(const VRegister& vn, const VRegister& vm) {
    Fcmp(vn, vm, EnableTrap);
  }
  void Fcsel(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             Condition cond) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT((cond != al) && (cond != nv));
    SingleEmissionCheckScope guard(this);
    fcsel(vd, vn, vm, cond);
  }
  void Fcvt(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvt(vd, vn);
  }
  void Fcvtl(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtl(vd, vn);
  }
  void Fcvtl2(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtl2(vd, vn);
  }
  void Fcvtn(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtn(vd, vn);
  }
  void Fcvtn2(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtn2(vd, vn);
  }
  void Fcvtxn(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtxn(vd, vn);
  }
  void Fcvtxn2(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtxn2(vd, vn);
  }
  void Fcvtas(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtas(rd, vn);
  }
  void Fcvtau(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtau(rd, vn);
  }
  void Fcvtms(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtms(rd, vn);
  }
  void Fcvtmu(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtmu(rd, vn);
  }
  void Fcvtns(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtns(rd, vn);
  }
  void Fcvtnu(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtnu(rd, vn);
  }
  void Fcvtps(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtps(rd, vn);
  }
  void Fcvtpu(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtpu(rd, vn);
  }
  void Fcvtzs(const Register& rd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtzs(rd, vn, fbits);
  }
  void Fjcvtzs(const Register& rd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fjcvtzs(rd, vn);
  }
  void Fcvtzu(const Register& rd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fcvtzu(rd, vn, fbits);
  }
  void Fdiv(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fdiv(vd, vn, vm);
  }
  void Fmax(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmax(vd, vn, vm);
  }
  void Fmaxnm(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmaxnm(vd, vn, vm);
  }
  void Fmin(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmin(vd, vn, vm);
  }
  void Fminnm(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fminnm(vd, vn, vm);
  }
  void Fmov(const VRegister& vd, const VRegister& vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    // Only emit an instruction if vd and vn are different, and they are both D
    // registers. fmov(s0, s0) is not a no-op because it clears the top word of
    // d0. Technically, fmov(d0, d0) is not a no-op either because it clears
    // the top of q0, but VRegister does not currently support Q registers.
    if (!vd.Is(vn) || !vd.Is64Bits()) {
      fmov(vd, vn);
    }
  }
  void Fmov(const VRegister& vd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    fmov(vd, rn);
  }
  void Fmov(const VRegister& vd, const XRegister& xn) {
    Fmov(vd, Register(xn));
  }
  void Fmov(const VRegister& vd, const WRegister& wn) {
    Fmov(vd, Register(wn));
  }
  void Fmov(const VRegister& vd, int index, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmov(vd, index, rn);
  }
  void Fmov(const Register& rd, const VRegister& vn, int index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmov(rd, vn, index);
  }

  // Provide explicit double and float interfaces for FP immediate moves, rather
  // than relying on implicit C++ casts. This allows signalling NaNs to be
  // preserved when the immediate matches the format of vd. Most systems convert
  // signalling NaNs to quiet NaNs when converting between float and double.
  void Fmov(VRegister vd, double imm);
  void Fmov(VRegister vd, float imm);
  void Fmov(VRegister vd, const Float16 imm);
  // Provide a template to allow other types to be converted automatically.
  template <typename T>
  void Fmov(VRegister vd, T imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    Fmov(vd, static_cast<double>(imm));
  }
  void Fmov(Register rd, VRegister vn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    fmov(rd, vn);
  }
  void Fmul(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmul(vd, vn, vm);
  }
  void Fnmul(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fnmul(vd, vn, vm);
  }
  void Fmadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmadd(vd, vn, vm, va);
  }
  void Fmsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fmsub(vd, vn, vm, va);
  }
  void Fnmadd(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fnmadd(vd, vn, vm, va);
  }
  void Fnmsub(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fnmsub(vd, vn, vm, va);
  }
  void Fsub(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fsub(vd, vn, vm);
  }
  void Hint(SystemHint code) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    hint(code);
  }
  void Hint(int imm7) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    hint(imm7);
  }
  void Hlt(int code) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    hlt(code);
  }
  void Isb() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    isb();
  }
  void Ldar(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldar(rt, src);
  }
  void Ldarb(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldarb(rt, src);
  }
  void Ldarh(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldarh(rt, src);
  }
  void Ldlar(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldlar(rt, src);
  }
  void Ldlarb(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldlarb(rt, src);
  }
  void Ldlarh(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldlarh(rt, src);
  }
  void Ldaxp(const Register& rt, const Register& rt2, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rt.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    ldaxp(rt, rt2, src);
  }
  void Ldaxr(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldaxr(rt, src);
  }
  void Ldaxrb(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldaxrb(rt, src);
  }
  void Ldaxrh(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldaxrh(rt, src);
  }

// clang-format off
#define COMPARE_AND_SWAP_SINGLE_MACRO_LIST(V) \
  V(cas,    Cas)                              \
  V(casa,   Casa)                             \
  V(casl,   Casl)                             \
  V(casal,  Casal)                            \
  V(casb,   Casb)                             \
  V(casab,  Casab)                            \
  V(caslb,  Caslb)                            \
  V(casalb, Casalb)                           \
  V(cash,   Cash)                             \
  V(casah,  Casah)                            \
  V(caslh,  Caslh)                            \
  V(casalh, Casalh)
// clang-format on

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)                                     \
  void MASM(const Register& rs, const Register& rt, const MemOperand& src) { \
    VIXL_ASSERT(allow_macro_instructions_);                                  \
    SingleEmissionCheckScope guard(this);                                    \
    ASM(rs, rt, src);                                                        \
  }
  COMPARE_AND_SWAP_SINGLE_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC


// clang-format off
#define COMPARE_AND_SWAP_PAIR_MACRO_LIST(V) \
  V(casp,   Casp)                           \
  V(caspa,  Caspa)                          \
  V(caspl,  Caspl)                          \
  V(caspal, Caspal)
// clang-format on

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)    \
  void MASM(const Register& rs,             \
            const Register& rs2,            \
            const Register& rt,             \
            const Register& rt2,            \
            const MemOperand& src) {        \
    VIXL_ASSERT(allow_macro_instructions_); \
    SingleEmissionCheckScope guard(this);   \
    ASM(rs, rs2, rt, rt2, src);             \
  }
  COMPARE_AND_SWAP_PAIR_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC

// These macros generate all the variations of the atomic memory operations,
// e.g. ldadd, ldadda, ldaddb, staddl, etc.

// clang-format off
#define ATOMIC_MEMORY_SIMPLE_MACRO_LIST(V, DEF, MASM_PRE, ASM_PRE) \
  V(DEF, MASM_PRE##add,  ASM_PRE##add)                             \
  V(DEF, MASM_PRE##clr,  ASM_PRE##clr)                             \
  V(DEF, MASM_PRE##eor,  ASM_PRE##eor)                             \
  V(DEF, MASM_PRE##set,  ASM_PRE##set)                             \
  V(DEF, MASM_PRE##smax, ASM_PRE##smax)                            \
  V(DEF, MASM_PRE##smin, ASM_PRE##smin)                            \
  V(DEF, MASM_PRE##umax, ASM_PRE##umax)                            \
  V(DEF, MASM_PRE##umin, ASM_PRE##umin)

#define ATOMIC_MEMORY_STORE_MACRO_MODES(V, MASM, ASM) \
  V(MASM,     ASM)                                    \
  V(MASM##l,  ASM##l)                                 \
  V(MASM##b,  ASM##b)                                 \
  V(MASM##lb, ASM##lb)                                \
  V(MASM##h,  ASM##h)                                 \
  V(MASM##lh, ASM##lh)

#define ATOMIC_MEMORY_LOAD_MACRO_MODES(V, MASM, ASM) \
  ATOMIC_MEMORY_STORE_MACRO_MODES(V, MASM, ASM)      \
  V(MASM##a,   ASM##a)                               \
  V(MASM##al,  ASM##al)                              \
  V(MASM##ab,  ASM##ab)                              \
  V(MASM##alb, ASM##alb)                             \
  V(MASM##ah,  ASM##ah)                              \
  V(MASM##alh, ASM##alh)
// clang-format on

#define DEFINE_MACRO_LOAD_ASM_FUNC(MASM, ASM)                                \
  void MASM(const Register& rs, const Register& rt, const MemOperand& src) { \
    VIXL_ASSERT(allow_macro_instructions_);                                  \
    SingleEmissionCheckScope guard(this);                                    \
    ASM(rs, rt, src);                                                        \
  }
#define DEFINE_MACRO_STORE_ASM_FUNC(MASM, ASM)           \
  void MASM(const Register& rs, const MemOperand& src) { \
    VIXL_ASSERT(allow_macro_instructions_);              \
    SingleEmissionCheckScope guard(this);                \
    ASM(rs, src);                                        \
  }

  ATOMIC_MEMORY_SIMPLE_MACRO_LIST(ATOMIC_MEMORY_LOAD_MACRO_MODES,
                                  DEFINE_MACRO_LOAD_ASM_FUNC,
                                  Ld,
                                  ld)
  ATOMIC_MEMORY_SIMPLE_MACRO_LIST(ATOMIC_MEMORY_STORE_MACRO_MODES,
                                  DEFINE_MACRO_STORE_ASM_FUNC,
                                  St,
                                  st)

#define DEFINE_MACRO_SWP_ASM_FUNC(MASM, ASM)                                 \
  void MASM(const Register& rs, const Register& rt, const MemOperand& src) { \
    VIXL_ASSERT(allow_macro_instructions_);                                  \
    SingleEmissionCheckScope guard(this);                                    \
    ASM(rs, rt, src);                                                        \
  }

  ATOMIC_MEMORY_LOAD_MACRO_MODES(DEFINE_MACRO_SWP_ASM_FUNC, Swp, swp)

#undef DEFINE_MACRO_LOAD_ASM_FUNC
#undef DEFINE_MACRO_STORE_ASM_FUNC
#undef DEFINE_MACRO_SWP_ASM_FUNC

  void Ldaprb(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldaprb(rt, src);
  }

  void Ldaprh(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldaprh(rt, src);
  }

  void Ldapr(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldapr(rt, src);
  }

  void Ldnp(const CPURegister& rt,
            const CPURegister& rt2,
            const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldnp(rt, rt2, src);
  }
  // Provide both double and float interfaces for FP immediate loads, rather
  // than relying on implicit C++ casts. This allows signalling NaNs to be
  // preserved when the immediate matches the format of fd. Most systems convert
  // signalling NaNs to quiet NaNs when converting between float and double.
  void Ldr(const VRegister& vt, double imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    RawLiteral* literal;
    if (vt.IsD()) {
      literal = new Literal<double>(imm,
                                    &literal_pool_,
                                    RawLiteral::kDeletedOnPlacementByPool);
    } else {
      literal = new Literal<float>(static_cast<float>(imm),
                                   &literal_pool_,
                                   RawLiteral::kDeletedOnPlacementByPool);
    }
    ldr(vt, literal);
  }
  void Ldr(const VRegister& vt, float imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    RawLiteral* literal;
    if (vt.IsS()) {
      literal = new Literal<float>(imm,
                                   &literal_pool_,
                                   RawLiteral::kDeletedOnPlacementByPool);
    } else {
      literal = new Literal<double>(static_cast<double>(imm),
                                    &literal_pool_,
                                    RawLiteral::kDeletedOnPlacementByPool);
    }
    ldr(vt, literal);
  }
  void Ldr(const VRegister& vt, uint64_t high64, uint64_t low64) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(vt.IsQ());
    SingleEmissionCheckScope guard(this);
    ldr(vt,
        new Literal<uint64_t>(high64,
                              low64,
                              &literal_pool_,
                              RawLiteral::kDeletedOnPlacementByPool));
  }
  void Ldr(const Register& rt, uint64_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    RawLiteral* literal;
    if (rt.Is64Bits()) {
      literal = new Literal<uint64_t>(imm,
                                      &literal_pool_,
                                      RawLiteral::kDeletedOnPlacementByPool);
    } else {
      VIXL_ASSERT(rt.Is32Bits());
      VIXL_ASSERT(IsUint32(imm) || IsInt32(imm));
      literal = new Literal<uint32_t>(static_cast<uint32_t>(imm),
                                      &literal_pool_,
                                      RawLiteral::kDeletedOnPlacementByPool);
    }
    ldr(rt, literal);
  }
  void Ldrsw(const Register& rt, uint32_t imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    ldrsw(rt,
          new Literal<uint32_t>(imm,
                                &literal_pool_,
                                RawLiteral::kDeletedOnPlacementByPool));
  }
  void Ldr(const CPURegister& rt, RawLiteral* literal) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldr(rt, literal);
  }
  void Ldrsw(const Register& rt, RawLiteral* literal) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldrsw(rt, literal);
  }
  void Ldxp(const Register& rt, const Register& rt2, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rt.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    ldxp(rt, rt2, src);
  }
  void Ldxr(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldxr(rt, src);
  }
  void Ldxrb(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldxrb(rt, src);
  }
  void Ldxrh(const Register& rt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ldxrh(rt, src);
  }
  void Lsl(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    lsl(rd, rn, shift);
  }
  void Lsl(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    lslv(rd, rn, rm);
  }
  void Lsr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    lsr(rd, rn, shift);
  }
  void Lsr(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    lsrv(rd, rn, rm);
  }
  void Madd(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    madd(rd, rn, rm, ra);
  }
  void Mneg(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    mneg(rd, rn, rm);
  }
  void Mov(const Register& rd,
           const Register& rn,
           DiscardMoveMode discard_mode = kDontDiscardForSameWReg) {
    VIXL_ASSERT(allow_macro_instructions_);
    // Emit a register move only if the registers are distinct, or if they are
    // not X registers.
    //
    // Note that mov(w0, w0) is not a no-op because it clears the top word of
    // x0. A flag is provided (kDiscardForSameWReg) if a move between the same W
    // registers is not required to clear the top word of the X register. In
    // this case, the instruction is discarded.
    //
    // If the sp is an operand, add #0 is emitted, otherwise, orr #0.
    if (!rd.Is(rn) ||
        (rd.Is32Bits() && (discard_mode == kDontDiscardForSameWReg))) {
      SingleEmissionCheckScope guard(this);
      mov(rd, rn);
    }
  }
  void Movk(const Register& rd, uint64_t imm, int shift = -1) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    SingleEmissionCheckScope guard(this);
    movk(rd, imm, shift);
  }
  void Mrs(const Register& rt, SystemRegister sysreg) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    mrs(rt, sysreg);
  }
  void Msr(SystemRegister sysreg, const Register& rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rt.IsZero());
    SingleEmissionCheckScope guard(this);
    msr(sysreg, rt);
  }
  void Sys(int op1, int crn, int crm, int op2, const Register& rt = xzr) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    sys(op1, crn, crm, op2, rt);
  }
  void Dc(DataCacheOp op, const Register& rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    dc(op, rt);
  }
  void Ic(InstructionCacheOp op, const Register& rt) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ic(op, rt);
  }
  void Msub(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    msub(rd, rn, rm, ra);
  }
  void Mul(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    mul(rd, rn, rm);
  }
  void Nop() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    nop();
  }
  void Rbit(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rbit(rd, rn);
  }
  void Ret(const Register& xn = lr) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!xn.IsZero());
    SingleEmissionCheckScope guard(this);
    ret(xn);
  }
  void Rev(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev(rd, rn);
  }
  void Rev16(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev16(rd, rn);
  }
  void Rev32(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev32(rd, rn);
  }
  void Rev64(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    rev64(rd, rn);
  }

#define PAUTH_MASM_VARIATIONS(V) \
  V(Paci, paci)                  \
  V(Pacd, pacd)                  \
  V(Auti, auti)                  \
  V(Autd, autd)

#define DEFINE_MACRO_ASM_FUNCS(MASM_PRE, ASM_PRE)            \
  void MASM_PRE##a(const Register& xd, const Register& xn) { \
    VIXL_ASSERT(allow_macro_instructions_);                  \
    SingleEmissionCheckScope guard(this);                    \
    ASM_PRE##a(xd, xn);                                      \
  }                                                          \
  void MASM_PRE##za(const Register& xd) {                    \
    VIXL_ASSERT(allow_macro_instructions_);                  \
    SingleEmissionCheckScope guard(this);                    \
    ASM_PRE##za(xd);                                         \
  }                                                          \
  void MASM_PRE##b(const Register& xd, const Register& xn) { \
    VIXL_ASSERT(allow_macro_instructions_);                  \
    SingleEmissionCheckScope guard(this);                    \
    ASM_PRE##b(xd, xn);                                      \
  }                                                          \
  void MASM_PRE##zb(const Register& xd) {                    \
    VIXL_ASSERT(allow_macro_instructions_);                  \
    SingleEmissionCheckScope guard(this);                    \
    ASM_PRE##zb(xd);                                         \
  }

  PAUTH_MASM_VARIATIONS(DEFINE_MACRO_ASM_FUNCS)
#undef DEFINE_MACRO_ASM_FUNCS

  void Pacga(const Register& xd, const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    pacga(xd, xn, xm);
  }

  void Xpaci(const Register& xd) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    xpaci(xd);
  }

  void Xpacd(const Register& xd) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    xpacd(xd);
  }
  void Ror(const Register& rd, const Register& rs, unsigned shift) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rs.IsZero());
    SingleEmissionCheckScope guard(this);
    ror(rd, rs, shift);
  }
  void Ror(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    rorv(rd, rn, rm);
  }
  void Sbfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sbfiz(rd, rn, lsb, width);
  }
  void Sbfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sbfm(rd, rn, immr, imms);
  }
  void Sbfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sbfx(rd, rn, lsb, width);
  }
  void Scvtf(const VRegister& vd, const Register& rn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    scvtf(vd, rn, fbits);
  }
  void Sdiv(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    sdiv(rd, rn, rm);
  }
  void Smaddl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    smaddl(rd, rn, rm, ra);
  }
  void Smsubl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    smsubl(rd, rn, rm, ra);
  }
  void Smull(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    smull(rd, rn, rm);
  }
  void Smulh(const Register& xd, const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!xd.IsZero());
    VIXL_ASSERT(!xn.IsZero());
    VIXL_ASSERT(!xm.IsZero());
    SingleEmissionCheckScope guard(this);
    smulh(xd, xn, xm);
  }
  void Stlr(const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stlr(rt, dst);
  }
  void Stlrb(const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stlrb(rt, dst);
  }
  void Stlrh(const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stlrh(rt, dst);
  }
  void Stllr(const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stllr(rt, dst);
  }
  void Stllrb(const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stllrb(rt, dst);
  }
  void Stllrh(const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stllrh(rt, dst);
  }
  void Stlxp(const Register& rs,
             const Register& rt,
             const Register& rt2,
             const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    VIXL_ASSERT(!rs.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    stlxp(rs, rt, rt2, dst);
  }
  void Stlxr(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stlxr(rs, rt, dst);
  }
  void Stlxrb(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stlxrb(rs, rt, dst);
  }
  void Stlxrh(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stlxrh(rs, rt, dst);
  }
  void Stnp(const CPURegister& rt,
            const CPURegister& rt2,
            const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    stnp(rt, rt2, dst);
  }
  void Stxp(const Register& rs,
            const Register& rt,
            const Register& rt2,
            const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    VIXL_ASSERT(!rs.Aliases(rt2));
    SingleEmissionCheckScope guard(this);
    stxp(rs, rt, rt2, dst);
  }
  void Stxr(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stxr(rs, rt, dst);
  }
  void Stxrb(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stxrb(rs, rt, dst);
  }
  void Stxrh(const Register& rs, const Register& rt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rs.Aliases(dst.GetBaseRegister()));
    VIXL_ASSERT(!rs.Aliases(rt));
    SingleEmissionCheckScope guard(this);
    stxrh(rs, rt, dst);
  }
  void Svc(int code) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    svc(code);
  }
  void Sxtb(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sxtb(rd, rn);
  }
  void Sxth(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sxth(rd, rn);
  }
  void Sxtw(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    sxtw(rd, rn);
  }
  void Tbl(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vm);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vn2, vm);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vn2, vn3, vm);
  }
  void Tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbl(vd, vn, vn2, vn3, vn4, vm);
  }
  void Tbx(const VRegister& vd, const VRegister& vn, const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vn2, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vn2, vn3, vm);
  }
  void Tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    tbx(vd, vn, vn2, vn3, vn4, vm);
  }
  void Tbnz(const Register& rt, unsigned bit_pos, Label* label);
  void Tbz(const Register& rt, unsigned bit_pos, Label* label);
  void Ubfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ubfiz(rd, rn, lsb, width);
  }
  void Ubfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ubfm(rd, rn, immr, imms);
  }
  void Ubfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ubfx(rd, rn, lsb, width);
  }
  void Ucvtf(const VRegister& vd, const Register& rn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    ucvtf(vd, rn, fbits);
  }
  void Udiv(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    udiv(rd, rn, rm);
  }
  void Umaddl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    umaddl(rd, rn, rm, ra);
  }
  void Umull(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    SingleEmissionCheckScope guard(this);
    umull(rd, rn, rm);
  }
  void Umulh(const Register& xd, const Register& xn, const Register& xm) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!xd.IsZero());
    VIXL_ASSERT(!xn.IsZero());
    VIXL_ASSERT(!xm.IsZero());
    SingleEmissionCheckScope guard(this);
    umulh(xd, xn, xm);
  }
  void Umsubl(const Register& rd,
              const Register& rn,
              const Register& rm,
              const Register& ra) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    VIXL_ASSERT(!rm.IsZero());
    VIXL_ASSERT(!ra.IsZero());
    SingleEmissionCheckScope guard(this);
    umsubl(rd, rn, rm, ra);
  }
  void Unreachable() {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    if (generate_simulator_code_) {
      hlt(kUnreachableOpcode);
    } else {
      // Branch to 0 to generate a segfault.
      // lr - kInstructionSize is the address of the offending instruction.
      blr(xzr);
    }
  }
  void Uxtb(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    uxtb(rd, rn);
  }
  void Uxth(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    uxth(rd, rn);
  }
  void Uxtw(const Register& rd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    VIXL_ASSERT(!rd.IsZero());
    VIXL_ASSERT(!rn.IsZero());
    SingleEmissionCheckScope guard(this);
    uxtw(rd, rn);
  }

// NEON 3 vector register instructions.
#define NEON_3VREG_MACRO_LIST(V) \
  V(add, Add)                    \
  V(addhn, Addhn)                \
  V(addhn2, Addhn2)              \
  V(addp, Addp)                  \
  V(and_, And)                   \
  V(bic, Bic)                    \
  V(bif, Bif)                    \
  V(bit, Bit)                    \
  V(bsl, Bsl)                    \
  V(cmeq, Cmeq)                  \
  V(cmge, Cmge)                  \
  V(cmgt, Cmgt)                  \
  V(cmhi, Cmhi)                  \
  V(cmhs, Cmhs)                  \
  V(cmtst, Cmtst)                \
  V(eor, Eor)                    \
  V(fabd, Fabd)                  \
  V(facge, Facge)                \
  V(facgt, Facgt)                \
  V(faddp, Faddp)                \
  V(fcmeq, Fcmeq)                \
  V(fcmge, Fcmge)                \
  V(fcmgt, Fcmgt)                \
  V(fmaxnmp, Fmaxnmp)            \
  V(fmaxp, Fmaxp)                \
  V(fminnmp, Fminnmp)            \
  V(fminp, Fminp)                \
  V(fmla, Fmla)                  \
  V(fmls, Fmls)                  \
  V(fmulx, Fmulx)                \
  V(frecps, Frecps)              \
  V(frsqrts, Frsqrts)            \
  V(mla, Mla)                    \
  V(mls, Mls)                    \
  V(mul, Mul)                    \
  V(orn, Orn)                    \
  V(orr, Orr)                    \
  V(pmul, Pmul)                  \
  V(pmull, Pmull)                \
  V(pmull2, Pmull2)              \
  V(raddhn, Raddhn)              \
  V(raddhn2, Raddhn2)            \
  V(rsubhn, Rsubhn)              \
  V(rsubhn2, Rsubhn2)            \
  V(saba, Saba)                  \
  V(sabal, Sabal)                \
  V(sabal2, Sabal2)              \
  V(sabd, Sabd)                  \
  V(sabdl, Sabdl)                \
  V(sabdl2, Sabdl2)              \
  V(saddl, Saddl)                \
  V(saddl2, Saddl2)              \
  V(saddw, Saddw)                \
  V(saddw2, Saddw2)              \
  V(shadd, Shadd)                \
  V(shsub, Shsub)                \
  V(smax, Smax)                  \
  V(smaxp, Smaxp)                \
  V(smin, Smin)                  \
  V(sminp, Sminp)                \
  V(smlal, Smlal)                \
  V(smlal2, Smlal2)              \
  V(smlsl, Smlsl)                \
  V(smlsl2, Smlsl2)              \
  V(smull, Smull)                \
  V(smull2, Smull2)              \
  V(sqadd, Sqadd)                \
  V(sqdmlal, Sqdmlal)            \
  V(sqdmlal2, Sqdmlal2)          \
  V(sqdmlsl, Sqdmlsl)            \
  V(sqdmlsl2, Sqdmlsl2)          \
  V(sqdmulh, Sqdmulh)            \
  V(sqdmull, Sqdmull)            \
  V(sqdmull2, Sqdmull2)          \
  V(sqrdmulh, Sqrdmulh)          \
  V(sdot, Sdot)                  \
  V(sqrdmlah, Sqrdmlah)          \
  V(udot, Udot)                  \
  V(sqrdmlsh, Sqrdmlsh)          \
  V(sqrshl, Sqrshl)              \
  V(sqshl, Sqshl)                \
  V(sqsub, Sqsub)                \
  V(srhadd, Srhadd)              \
  V(srshl, Srshl)                \
  V(sshl, Sshl)                  \
  V(ssubl, Ssubl)                \
  V(ssubl2, Ssubl2)              \
  V(ssubw, Ssubw)                \
  V(ssubw2, Ssubw2)              \
  V(sub, Sub)                    \
  V(subhn, Subhn)                \
  V(subhn2, Subhn2)              \
  V(trn1, Trn1)                  \
  V(trn2, Trn2)                  \
  V(uaba, Uaba)                  \
  V(uabal, Uabal)                \
  V(uabal2, Uabal2)              \
  V(uabd, Uabd)                  \
  V(uabdl, Uabdl)                \
  V(uabdl2, Uabdl2)              \
  V(uaddl, Uaddl)                \
  V(uaddl2, Uaddl2)              \
  V(uaddw, Uaddw)                \
  V(uaddw2, Uaddw2)              \
  V(uhadd, Uhadd)                \
  V(uhsub, Uhsub)                \
  V(umax, Umax)                  \
  V(umaxp, Umaxp)                \
  V(umin, Umin)                  \
  V(uminp, Uminp)                \
  V(umlal, Umlal)                \
  V(umlal2, Umlal2)              \
  V(umlsl, Umlsl)                \
  V(umlsl2, Umlsl2)              \
  V(umull, Umull)                \
  V(umull2, Umull2)              \
  V(uqadd, Uqadd)                \
  V(uqrshl, Uqrshl)              \
  V(uqshl, Uqshl)                \
  V(uqsub, Uqsub)                \
  V(urhadd, Urhadd)              \
  V(urshl, Urshl)                \
  V(ushl, Ushl)                  \
  V(usubl, Usubl)                \
  V(usubl2, Usubl2)              \
  V(usubw, Usubw)                \
  V(usubw2, Usubw2)              \
  V(uzp1, Uzp1)                  \
  V(uzp2, Uzp2)                  \
  V(zip1, Zip1)                  \
  V(zip2, Zip2)

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)                                     \
  void MASM(const VRegister& vd, const VRegister& vn, const VRegister& vm) { \
    VIXL_ASSERT(allow_macro_instructions_);                                  \
    SingleEmissionCheckScope guard(this);                                    \
    ASM(vd, vn, vm);                                                         \
  }
  NEON_3VREG_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC

// NEON 2 vector register instructions.
#define NEON_2VREG_MACRO_LIST(V) \
  V(abs, Abs)                    \
  V(addp, Addp)                  \
  V(addv, Addv)                  \
  V(cls, Cls)                    \
  V(clz, Clz)                    \
  V(cnt, Cnt)                    \
  V(fabs, Fabs)                  \
  V(faddp, Faddp)                \
  V(fcvtas, Fcvtas)              \
  V(fcvtau, Fcvtau)              \
  V(fcvtms, Fcvtms)              \
  V(fcvtmu, Fcvtmu)              \
  V(fcvtns, Fcvtns)              \
  V(fcvtnu, Fcvtnu)              \
  V(fcvtps, Fcvtps)              \
  V(fcvtpu, Fcvtpu)              \
  V(fmaxnmp, Fmaxnmp)            \
  V(fmaxnmv, Fmaxnmv)            \
  V(fmaxp, Fmaxp)                \
  V(fmaxv, Fmaxv)                \
  V(fminnmp, Fminnmp)            \
  V(fminnmv, Fminnmv)            \
  V(fminp, Fminp)                \
  V(fminv, Fminv)                \
  V(fneg, Fneg)                  \
  V(frecpe, Frecpe)              \
  V(frecpx, Frecpx)              \
  V(frinta, Frinta)              \
  V(frinti, Frinti)              \
  V(frintm, Frintm)              \
  V(frintn, Frintn)              \
  V(frintp, Frintp)              \
  V(frintx, Frintx)              \
  V(frintz, Frintz)              \
  V(frsqrte, Frsqrte)            \
  V(fsqrt, Fsqrt)                \
  V(mov, Mov)                    \
  V(mvn, Mvn)                    \
  V(neg, Neg)                    \
  V(not_, Not)                   \
  V(rbit, Rbit)                  \
  V(rev16, Rev16)                \
  V(rev32, Rev32)                \
  V(rev64, Rev64)                \
  V(sadalp, Sadalp)              \
  V(saddlp, Saddlp)              \
  V(saddlv, Saddlv)              \
  V(smaxv, Smaxv)                \
  V(sminv, Sminv)                \
  V(sqabs, Sqabs)                \
  V(sqneg, Sqneg)                \
  V(sqxtn, Sqxtn)                \
  V(sqxtn2, Sqxtn2)              \
  V(sqxtun, Sqxtun)              \
  V(sqxtun2, Sqxtun2)            \
  V(suqadd, Suqadd)              \
  V(sxtl, Sxtl)                  \
  V(sxtl2, Sxtl2)                \
  V(uadalp, Uadalp)              \
  V(uaddlp, Uaddlp)              \
  V(uaddlv, Uaddlv)              \
  V(umaxv, Umaxv)                \
  V(uminv, Uminv)                \
  V(uqxtn, Uqxtn)                \
  V(uqxtn2, Uqxtn2)              \
  V(urecpe, Urecpe)              \
  V(ursqrte, Ursqrte)            \
  V(usqadd, Usqadd)              \
  V(uxtl, Uxtl)                  \
  V(uxtl2, Uxtl2)                \
  V(xtn, Xtn)                    \
  V(xtn2, Xtn2)

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)                \
  void MASM(const VRegister& vd, const VRegister& vn) { \
    VIXL_ASSERT(allow_macro_instructions_);             \
    SingleEmissionCheckScope guard(this);               \
    ASM(vd, vn);                                        \
  }
  NEON_2VREG_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC

// NEON 2 vector register with immediate instructions.
#define NEON_2VREG_FPIMM_MACRO_LIST(V) \
  V(fcmeq, Fcmeq)                      \
  V(fcmge, Fcmge)                      \
  V(fcmgt, Fcmgt)                      \
  V(fcmle, Fcmle)                      \
  V(fcmlt, Fcmlt)

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)                            \
  void MASM(const VRegister& vd, const VRegister& vn, double imm) { \
    VIXL_ASSERT(allow_macro_instructions_);                         \
    SingleEmissionCheckScope guard(this);                           \
    ASM(vd, vn, imm);                                               \
  }
  NEON_2VREG_FPIMM_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC

// NEON by element instructions.
#define NEON_BYELEMENT_MACRO_LIST(V) \
  V(fmul, Fmul)                      \
  V(fmla, Fmla)                      \
  V(fmls, Fmls)                      \
  V(fmulx, Fmulx)                    \
  V(mul, Mul)                        \
  V(mla, Mla)                        \
  V(mls, Mls)                        \
  V(sqdmulh, Sqdmulh)                \
  V(sqrdmulh, Sqrdmulh)              \
  V(sdot, Sdot)                      \
  V(sqrdmlah, Sqrdmlah)              \
  V(udot, Udot)                      \
  V(sqrdmlsh, Sqrdmlsh)              \
  V(sqdmull, Sqdmull)                \
  V(sqdmull2, Sqdmull2)              \
  V(sqdmlal, Sqdmlal)                \
  V(sqdmlal2, Sqdmlal2)              \
  V(sqdmlsl, Sqdmlsl)                \
  V(sqdmlsl2, Sqdmlsl2)              \
  V(smull, Smull)                    \
  V(smull2, Smull2)                  \
  V(smlal, Smlal)                    \
  V(smlal2, Smlal2)                  \
  V(smlsl, Smlsl)                    \
  V(smlsl2, Smlsl2)                  \
  V(umull, Umull)                    \
  V(umull2, Umull2)                  \
  V(umlal, Umlal)                    \
  V(umlal2, Umlal2)                  \
  V(umlsl, Umlsl)                    \
  V(umlsl2, Umlsl2)

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)    \
  void MASM(const VRegister& vd,            \
            const VRegister& vn,            \
            const VRegister& vm,            \
            int vm_index) {                 \
    VIXL_ASSERT(allow_macro_instructions_); \
    SingleEmissionCheckScope guard(this);   \
    ASM(vd, vn, vm, vm_index);              \
  }
  NEON_BYELEMENT_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC

#define NEON_2VREG_SHIFT_MACRO_LIST(V) \
  V(rshrn, Rshrn)                      \
  V(rshrn2, Rshrn2)                    \
  V(shl, Shl)                          \
  V(shll, Shll)                        \
  V(shll2, Shll2)                      \
  V(shrn, Shrn)                        \
  V(shrn2, Shrn2)                      \
  V(sli, Sli)                          \
  V(sqrshrn, Sqrshrn)                  \
  V(sqrshrn2, Sqrshrn2)                \
  V(sqrshrun, Sqrshrun)                \
  V(sqrshrun2, Sqrshrun2)              \
  V(sqshl, Sqshl)                      \
  V(sqshlu, Sqshlu)                    \
  V(sqshrn, Sqshrn)                    \
  V(sqshrn2, Sqshrn2)                  \
  V(sqshrun, Sqshrun)                  \
  V(sqshrun2, Sqshrun2)                \
  V(sri, Sri)                          \
  V(srshr, Srshr)                      \
  V(srsra, Srsra)                      \
  V(sshll, Sshll)                      \
  V(sshll2, Sshll2)                    \
  V(sshr, Sshr)                        \
  V(ssra, Ssra)                        \
  V(uqrshrn, Uqrshrn)                  \
  V(uqrshrn2, Uqrshrn2)                \
  V(uqshl, Uqshl)                      \
  V(uqshrn, Uqshrn)                    \
  V(uqshrn2, Uqshrn2)                  \
  V(urshr, Urshr)                      \
  V(ursra, Ursra)                      \
  V(ushll, Ushll)                      \
  V(ushll2, Ushll2)                    \
  V(ushr, Ushr)                        \
  V(usra, Usra)

#define DEFINE_MACRO_ASM_FUNC(ASM, MASM)                           \
  void MASM(const VRegister& vd, const VRegister& vn, int shift) { \
    VIXL_ASSERT(allow_macro_instructions_);                        \
    SingleEmissionCheckScope guard(this);                          \
    ASM(vd, vn, shift);                                            \
  }
  NEON_2VREG_SHIFT_MACRO_LIST(DEFINE_MACRO_ASM_FUNC)
#undef DEFINE_MACRO_ASM_FUNC

  void Bic(const VRegister& vd, const int imm8, const int left_shift = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    bic(vd, imm8, left_shift);
  }
  void Cmeq(const VRegister& vd, const VRegister& vn, int imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    cmeq(vd, vn, imm);
  }
  void Cmge(const VRegister& vd, const VRegister& vn, int imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    cmge(vd, vn, imm);
  }
  void Cmgt(const VRegister& vd, const VRegister& vn, int imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    cmgt(vd, vn, imm);
  }
  void Cmle(const VRegister& vd, const VRegister& vn, int imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    cmle(vd, vn, imm);
  }
  void Cmlt(const VRegister& vd, const VRegister& vn, int imm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    cmlt(vd, vn, imm);
  }
  void Dup(const VRegister& vd, const VRegister& vn, int index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    dup(vd, vn, index);
  }
  void Dup(const VRegister& vd, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    dup(vd, rn);
  }
  void Ext(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ext(vd, vn, vm, index);
  }
  void Fcadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int rot) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcadd(vd, vn, vm, rot);
  }
  void Fcmla(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index,
             int rot) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcmla(vd, vn, vm, vm_index, rot);
  }
  void Fcmla(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int rot) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcmla(vd, vn, vm, rot);
  }
  void Ins(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ins(vd, vd_index, vn, vn_index);
  }
  void Ins(const VRegister& vd, int vd_index, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ins(vd, vd_index, rn);
  }
  void Ld1(const VRegister& vt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld1(vt, src);
  }
  void Ld1(const VRegister& vt, const VRegister& vt2, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld1(vt, vt2, src);
  }
  void Ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld1(vt, vt2, vt3, src);
  }
  void Ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld1(vt, vt2, vt3, vt4, src);
  }
  void Ld1(const VRegister& vt, int lane, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld1(vt, lane, src);
  }
  void Ld1r(const VRegister& vt, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld1r(vt, src);
  }
  void Ld2(const VRegister& vt, const VRegister& vt2, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld2(vt, vt2, src);
  }
  void Ld2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld2(vt, vt2, lane, src);
  }
  void Ld2r(const VRegister& vt, const VRegister& vt2, const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld2r(vt, vt2, src);
  }
  void Ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld3(vt, vt2, vt3, src);
  }
  void Ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld3(vt, vt2, vt3, lane, src);
  }
  void Ld3r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld3r(vt, vt2, vt3, src);
  }
  void Ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld4(vt, vt2, vt3, vt4, src);
  }
  void Ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld4(vt, vt2, vt3, vt4, lane, src);
  }
  void Ld4r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const VRegister& vt4,
            const MemOperand& src) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ld4r(vt, vt2, vt3, vt4, src);
  }
  void Mov(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    mov(vd, vd_index, vn, vn_index);
  }
  void Mov(const VRegister& vd, const VRegister& vn, int index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    mov(vd, vn, index);
  }
  void Mov(const VRegister& vd, int vd_index, const Register& rn) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    mov(vd, vd_index, rn);
  }
  void Mov(const Register& rd, const VRegister& vn, int vn_index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    mov(rd, vn, vn_index);
  }
  void Movi(const VRegister& vd,
            uint64_t imm,
            Shift shift = LSL,
            int shift_amount = 0);
  void Movi(const VRegister& vd, uint64_t hi, uint64_t lo);
  void Mvni(const VRegister& vd,
            const int imm8,
            Shift shift = LSL,
            const int shift_amount = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    mvni(vd, imm8, shift, shift_amount);
  }
  void Orr(const VRegister& vd, const int imm8, const int left_shift = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    orr(vd, imm8, left_shift);
  }
  void Scvtf(const VRegister& vd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    scvtf(vd, vn, fbits);
  }
  void Ucvtf(const VRegister& vd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    ucvtf(vd, vn, fbits);
  }
  void Fcvtzs(const VRegister& vd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtzs(vd, vn, fbits);
  }
  void Fcvtzu(const VRegister& vd, const VRegister& vn, int fbits = 0) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    fcvtzu(vd, vn, fbits);
  }
  void St1(const VRegister& vt, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st1(vt, dst);
  }
  void St1(const VRegister& vt, const VRegister& vt2, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st1(vt, vt2, dst);
  }
  void St1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st1(vt, vt2, vt3, dst);
  }
  void St1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st1(vt, vt2, vt3, vt4, dst);
  }
  void St1(const VRegister& vt, int lane, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st1(vt, lane, dst);
  }
  void St2(const VRegister& vt, const VRegister& vt2, const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st2(vt, vt2, dst);
  }
  void St3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st3(vt, vt2, vt3, dst);
  }
  void St4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st4(vt, vt2, vt3, vt4, dst);
  }
  void St2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st2(vt, vt2, lane, dst);
  }
  void St3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st3(vt, vt2, vt3, lane, dst);
  }
  void St4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& dst) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    st4(vt, vt2, vt3, vt4, lane, dst);
  }
  void Smov(const Register& rd, const VRegister& vn, int vn_index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    smov(rd, vn, vn_index);
  }
  void Umov(const Register& rd, const VRegister& vn, int vn_index) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    umov(rd, vn, vn_index);
  }
  void Crc32b(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32b(rd, rn, rm);
  }
  void Crc32h(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32h(rd, rn, rm);
  }
  void Crc32w(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32w(rd, rn, rm);
  }
  void Crc32x(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32x(rd, rn, rm);
  }
  void Crc32cb(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32cb(rd, rn, rm);
  }
  void Crc32ch(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32ch(rd, rn, rm);
  }
  void Crc32cw(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32cw(rd, rn, rm);
  }
  void Crc32cx(const Register& rd, const Register& rn, const Register& rm) {
    VIXL_ASSERT(allow_macro_instructions_);
    SingleEmissionCheckScope guard(this);
    crc32cx(rd, rn, rm);
  }

  template <typename T>
  Literal<T>* CreateLiteralDestroyedWithPool(T value) {
    return new Literal<T>(value,
                          &literal_pool_,
                          RawLiteral::kDeletedOnPoolDestruction);
  }

  template <typename T>
  Literal<T>* CreateLiteralDestroyedWithPool(T high64, T low64) {
    return new Literal<T>(high64,
                          low64,
                          &literal_pool_,
                          RawLiteral::kDeletedOnPoolDestruction);
  }

  // Push the system stack pointer (sp) down to allow the same to be done to
  // the current stack pointer (according to StackPointer()). This must be
  // called _before_ accessing the memory.
  //
  // This is necessary when pushing or otherwise adding things to the stack, to
  // satisfy the AAPCS64 constraint that the memory below the system stack
  // pointer is not accessed.
  //
  // This method asserts that StackPointer() is not sp, since the call does
  // not make sense in that context.
  //
  // TODO: This method can only accept values of 'space' that can be encoded in
  // one instruction. Refer to the implementation for details.
  void BumpSystemStackPointer(const Operand& space);

  virtual bool AllowMacroInstructions() const VIXL_OVERRIDE {
    return allow_macro_instructions_;
  }

  virtual bool ArePoolsBlocked() const VIXL_OVERRIDE {
    return IsLiteralPoolBlocked() && IsVeneerPoolBlocked();
  }

  void SetGenerateSimulatorCode(bool value) {
    generate_simulator_code_ = value;
  }

  bool GenerateSimulatorCode() const { return generate_simulator_code_; }

  size_t GetLiteralPoolSize() const { return literal_pool_.GetSize(); }
  VIXL_DEPRECATED("GetLiteralPoolSize", size_t LiteralPoolSize() const) {
    return GetLiteralPoolSize();
  }

  size_t GetLiteralPoolMaxSize() const { return literal_pool_.GetMaxSize(); }
  VIXL_DEPRECATED("GetLiteralPoolMaxSize", size_t LiteralPoolMaxSize() const) {
    return GetLiteralPoolMaxSize();
  }

  size_t GetVeneerPoolMaxSize() const { return veneer_pool_.GetMaxSize(); }
  VIXL_DEPRECATED("GetVeneerPoolMaxSize", size_t VeneerPoolMaxSize() const) {
    return GetVeneerPoolMaxSize();
  }

  // The number of unresolved branches that may require a veneer.
  int GetNumberOfPotentialVeneers() const {
    return veneer_pool_.GetNumberOfPotentialVeneers();
  }
  VIXL_DEPRECATED("GetNumberOfPotentialVeneers",
                  int NumberOfPotentialVeneers() const) {
    return GetNumberOfPotentialVeneers();
  }

  ptrdiff_t GetNextCheckPoint() const {
    ptrdiff_t next_checkpoint_for_pools =
        std::min(literal_pool_.GetCheckpoint(), veneer_pool_.GetCheckpoint());
    return std::min(next_checkpoint_for_pools,
                    static_cast<ptrdiff_t>(GetBuffer().GetCapacity()));
  }
  VIXL_DEPRECATED("GetNextCheckPoint", ptrdiff_t NextCheckPoint()) {
    return GetNextCheckPoint();
  }

  void EmitLiteralPool(LiteralPool::EmitOption option) {
    if (!literal_pool_.IsEmpty()) literal_pool_.Emit(option);

    checkpoint_ = GetNextCheckPoint();
    recommended_checkpoint_ = literal_pool_.GetNextRecommendedCheckpoint();
  }

  void CheckEmitFor(size_t amount);
  void EnsureEmitFor(size_t amount) {
    ptrdiff_t offset = amount;
    ptrdiff_t max_pools_size =
        literal_pool_.GetMaxSize() + veneer_pool_.GetMaxSize();
    ptrdiff_t cursor = GetCursorOffset();
    if ((cursor >= recommended_checkpoint_) ||
        ((cursor + offset + max_pools_size) >= checkpoint_)) {
      CheckEmitFor(amount);
    }
  }

  void CheckEmitPoolsFor(size_t amount);
  virtual void EnsureEmitPoolsFor(size_t amount) VIXL_OVERRIDE {
    ptrdiff_t offset = amount;
    ptrdiff_t max_pools_size =
        literal_pool_.GetMaxSize() + veneer_pool_.GetMaxSize();
    ptrdiff_t cursor = GetCursorOffset();
    if ((cursor >= recommended_checkpoint_) ||
        ((cursor + offset + max_pools_size) >= checkpoint_)) {
      CheckEmitPoolsFor(amount);
    }
  }

  // Set the current stack pointer, but don't generate any code.
  void SetStackPointer(const Register& stack_pointer) {
    VIXL_ASSERT(!GetScratchRegisterList()->IncludesAliasOf(stack_pointer));
    sp_ = stack_pointer;
  }

  // Return the current stack pointer, as set by SetStackPointer.
  const Register& StackPointer() const { return sp_; }

  CPURegList* GetScratchRegisterList() { return &tmp_list_; }
  VIXL_DEPRECATED("GetScratchRegisterList", CPURegList* TmpList()) {
    return GetScratchRegisterList();
  }

  CPURegList* GetScratchFPRegisterList() { return &fptmp_list_; }
  VIXL_DEPRECATED("GetScratchFPRegisterList", CPURegList* FPTmpList()) {
    return GetScratchFPRegisterList();
  }

  // Get or set the current (most-deeply-nested) UseScratchRegisterScope.
  void SetCurrentScratchRegisterScope(UseScratchRegisterScope* scope) {
    current_scratch_scope_ = scope;
  }
  UseScratchRegisterScope* GetCurrentScratchRegisterScope() {
    return current_scratch_scope_;
  }

  // Like printf, but print at run-time from generated code.
  //
  // The caller must ensure that arguments for floating-point placeholders
  // (such as %e, %f or %g) are VRegisters in format 1S or 1D, and that
  // arguments for integer placeholders are Registers.
  //
  // At the moment it is only possible to print the value of sp if it is the
  // current stack pointer. Otherwise, the MacroAssembler will automatically
  // update sp on every push (using BumpSystemStackPointer), so determining its
  // value is difficult.
  //
  // Format placeholders that refer to more than one argument, or to a specific
  // argument, are not supported. This includes formats like "%1$d" or "%.*d".
  //
  // This function automatically preserves caller-saved registers so that
  // calling code can use Printf at any point without having to worry about
  // corruption. The preservation mechanism generates a lot of code. If this is
  // a problem, preserve the important registers manually and then call
  // PrintfNoPreserve. Callee-saved registers are not used by Printf, and are
  // implicitly preserved.
  void Printf(const char* format,
              CPURegister arg0 = NoCPUReg,
              CPURegister arg1 = NoCPUReg,
              CPURegister arg2 = NoCPUReg,
              CPURegister arg3 = NoCPUReg);

  // Like Printf, but don't preserve any caller-saved registers, not even 'lr'.
  //
  // The return code from the system printf call will be returned in x0.
  void PrintfNoPreserve(const char* format,
                        const CPURegister& arg0 = NoCPUReg,
                        const CPURegister& arg1 = NoCPUReg,
                        const CPURegister& arg2 = NoCPUReg,
                        const CPURegister& arg3 = NoCPUReg);

  // Trace control when running the debug simulator.
  //
  // For example:
  //
  // __ Trace(LOG_REGS, TRACE_ENABLE);
  // Will add registers to the trace if it wasn't already the case.
  //
  // __ Trace(LOG_DISASM, TRACE_DISABLE);
  // Will stop logging disassembly. It has no effect if the disassembly wasn't
  // already being logged.
  void Trace(TraceParameters parameters, TraceCommand command);

  // Log the requested data independently of what is being traced.
  //
  // For example:
  //
  // __ Log(LOG_FLAGS)
  // Will output the flags.
  void Log(TraceParameters parameters);

  // Enable or disable instrumentation when an Instrument visitor is attached to
  // the simulator.
  void EnableInstrumentation();
  void DisableInstrumentation();

  // Add a marker to the instrumentation data produced by an Instrument visitor.
  // The name is a two character string that will be attached to the marker in
  // the output data.
  void AnnotateInstrumentation(const char* marker_name);

  // Enable or disable CPU features dynamically. This mechanism allows users to
  // strictly check the use of CPU features in different regions of code.
  void SetSimulatorCPUFeatures(const CPUFeatures& features);
  void EnableSimulatorCPUFeatures(const CPUFeatures& features);
  void DisableSimulatorCPUFeatures(const CPUFeatures& features);
  void SaveSimulatorCPUFeatures();
  void RestoreSimulatorCPUFeatures();

  LiteralPool* GetLiteralPool() { return &literal_pool_; }

// Support for simulated runtime calls.

// `CallRuntime` requires variadic templating, that is only available from
// C++11.
#if __cplusplus >= 201103L
#define VIXL_HAS_MACROASSEMBLER_RUNTIME_CALL_SUPPORT
#endif  // #if __cplusplus >= 201103L

#ifdef VIXL_HAS_MACROASSEMBLER_RUNTIME_CALL_SUPPORT
  template <typename R, typename... P>
  void CallRuntimeHelper(R (*function)(P...), RuntimeCallType call_type);

  template <typename R, typename... P>
  void CallRuntime(R (*function)(P...)) {
    CallRuntimeHelper(function, kCallRuntime);
  }

  template <typename R, typename... P>
  void TailCallRuntime(R (*function)(P...)) {
    CallRuntimeHelper(function, kTailCallRuntime);
  }
#endif  // #ifdef VIXL_HAS_MACROASSEMBLER_RUNTIME_CALL_SUPPORT

 protected:
  void BlockLiteralPool() { literal_pool_.Block(); }
  void ReleaseLiteralPool() { literal_pool_.Release(); }
  bool IsLiteralPoolBlocked() const { return literal_pool_.IsBlocked(); }
  void BlockVeneerPool() { veneer_pool_.Block(); }
  void ReleaseVeneerPool() { veneer_pool_.Release(); }
  bool IsVeneerPoolBlocked() const { return veneer_pool_.IsBlocked(); }

  virtual void BlockPools() VIXL_OVERRIDE {
    BlockLiteralPool();
    BlockVeneerPool();
  }

  virtual void ReleasePools() VIXL_OVERRIDE {
    ReleaseLiteralPool();
    ReleaseVeneerPool();
  }

  // The scopes below need to able to block and release a particular pool.
  // TODO: Consider removing those scopes or move them to
  // code-generation-scopes-vixl.h.
  friend class BlockPoolsScope;
  friend class BlockLiteralPoolScope;
  friend class BlockVeneerPoolScope;

  virtual void SetAllowMacroInstructions(bool value) VIXL_OVERRIDE {
    allow_macro_instructions_ = value;
  }

  // Helper used to query information about code generation and to generate
  // code for `csel`.
  // Here and for the related helpers below:
  // - Code is generated when `masm` is not `NULL`.
  // - On return and when set, `should_synthesise_left` and
  //   `should_synthesise_right` will indicate whether `left` and `right`
  //   should be synthesized in a temporary register.
  static void CselHelper(MacroAssembler* masm,
                         const Register& rd,
                         Operand left,
                         Operand right,
                         Condition cond,
                         bool* should_synthesise_left = NULL,
                         bool* should_synthesise_right = NULL);

  // The helper returns `true` if it can handle the specified arguments.
  // Also see comments for `CselHelper()`.
  static bool CselSubHelperTwoImmediates(MacroAssembler* masm,
                                         const Register& rd,
                                         int64_t left,
                                         int64_t right,
                                         Condition cond,
                                         bool* should_synthesise_left,
                                         bool* should_synthesise_right);

  // See comments for `CselHelper()`.
  static bool CselSubHelperTwoOrderedImmediates(MacroAssembler* masm,
                                                const Register& rd,
                                                int64_t left,
                                                int64_t right,
                                                Condition cond);

  // See comments for `CselHelper()`.
  static void CselSubHelperRightSmallImmediate(MacroAssembler* masm,
                                               UseScratchRegisterScope* temps,
                                               const Register& rd,
                                               const Operand& left,
                                               const Operand& right,
                                               Condition cond,
                                               bool* should_synthesise_left);

 private:
  // The actual Push and Pop implementations. These don't generate any code
  // other than that required for the push or pop. This allows
  // (Push|Pop)CPURegList to bundle together setup code for a large block of
  // registers.
  //
  // Note that size is per register, and is specified in bytes.
  void PushHelper(int count,
                  int size,
                  const CPURegister& src0,
                  const CPURegister& src1,
                  const CPURegister& src2,
                  const CPURegister& src3);
  void PopHelper(int count,
                 int size,
                 const CPURegister& dst0,
                 const CPURegister& dst1,
                 const CPURegister& dst2,
                 const CPURegister& dst3);

  void Movi16bitHelper(const VRegister& vd, uint64_t imm);
  void Movi32bitHelper(const VRegister& vd, uint64_t imm);
  void Movi64bitHelper(const VRegister& vd, uint64_t imm);

  // Perform necessary maintenance operations before a push or pop.
  //
  // Note that size is per register, and is specified in bytes.
  void PrepareForPush(int count, int size);
  void PrepareForPop(int count, int size);

  // The actual implementation of load and store operations for CPURegList.
  enum LoadStoreCPURegListAction { kLoad, kStore };
  void LoadStoreCPURegListHelper(LoadStoreCPURegListAction operation,
                                 CPURegList registers,
                                 const MemOperand& mem);
  // Returns a MemOperand suitable for loading or storing a CPURegList at `dst`.
  // This helper may allocate registers from `scratch_scope` and generate code
  // to compute an intermediate address. The resulting MemOperand is only valid
  // as long as `scratch_scope` remains valid.
  MemOperand BaseMemOperandForLoadStoreCPURegList(
      const CPURegList& registers,
      const MemOperand& mem,
      UseScratchRegisterScope* scratch_scope);

  bool LabelIsOutOfRange(Label* label, ImmBranchType branch_type) {
    return !Instruction::IsValidImmPCOffset(branch_type,
                                            label->GetLocation() -
                                                GetCursorOffset());
  }

  void ConfigureSimulatorCPUFeaturesHelper(const CPUFeatures& features,
                                           DebugHltOpcode action);

  // Tell whether any of the macro instruction can be used. When false the
  // MacroAssembler will assert if a method which can emit a variable number
  // of instructions is called.
  bool allow_macro_instructions_;

  // Indicates whether we should generate simulator or native code.
  bool generate_simulator_code_;

  // The register to use as a stack pointer for stack operations.
  Register sp_;

  // Scratch registers available for use by the MacroAssembler.
  CPURegList tmp_list_;
  CPURegList fptmp_list_;

  UseScratchRegisterScope* current_scratch_scope_;

  LiteralPool literal_pool_;
  VeneerPool veneer_pool_;

  ptrdiff_t checkpoint_;
  ptrdiff_t recommended_checkpoint_;

  friend class Pool;
  friend class LiteralPool;
};


inline size_t VeneerPool::GetOtherPoolsMaxSize() const {
  return masm_->GetLiteralPoolMaxSize();
}


inline size_t LiteralPool::GetOtherPoolsMaxSize() const {
  return masm_->GetVeneerPoolMaxSize();
}


inline void LiteralPool::SetNextRecommendedCheckpoint(ptrdiff_t offset) {
  masm_->recommended_checkpoint_ =
      std::min(masm_->recommended_checkpoint_, offset);
  recommended_checkpoint_ = offset;
}

class InstructionAccurateScope : public ExactAssemblyScope {
 public:
  VIXL_DEPRECATED("ExactAssemblyScope",
                  InstructionAccurateScope(MacroAssembler* masm,
                                           int64_t count,
                                           SizePolicy size_policy = kExactSize))
      : ExactAssemblyScope(masm, count * kInstructionSize, size_policy) {}
};

class BlockLiteralPoolScope {
 public:
  explicit BlockLiteralPoolScope(MacroAssembler* masm) : masm_(masm) {
    masm_->BlockLiteralPool();
  }

  ~BlockLiteralPoolScope() { masm_->ReleaseLiteralPool(); }

 private:
  MacroAssembler* masm_;
};


class BlockVeneerPoolScope {
 public:
  explicit BlockVeneerPoolScope(MacroAssembler* masm) : masm_(masm) {
    masm_->BlockVeneerPool();
  }

  ~BlockVeneerPoolScope() { masm_->ReleaseVeneerPool(); }

 private:
  MacroAssembler* masm_;
};


class BlockPoolsScope {
 public:
  explicit BlockPoolsScope(MacroAssembler* masm) : masm_(masm) {
    masm_->BlockPools();
  }

  ~BlockPoolsScope() { masm_->ReleasePools(); }

 private:
  MacroAssembler* masm_;
};


// This scope utility allows scratch registers to be managed safely. The
// MacroAssembler's GetScratchRegisterList() (and GetScratchFPRegisterList()) is
// used as a pool of scratch registers. These registers can be allocated on
// demand, and will be returned at the end of the scope.
//
// When the scope ends, the MacroAssembler's lists will be restored to their
// original state, even if the lists were modified by some other means.
class UseScratchRegisterScope {
 public:
  // This constructor implicitly calls `Open` to initialise the scope (`masm`
  // must not be `NULL`), so it is ready to use immediately after it has been
  // constructed.
  explicit UseScratchRegisterScope(MacroAssembler* masm)
      : masm_(NULL), parent_(NULL), old_available_(0), old_availablefp_(0) {
    Open(masm);
  }
  // This constructor does not implicitly initialise the scope. Instead, the
  // user is required to explicitly call the `Open` function before using the
  // scope.
  UseScratchRegisterScope()
      : masm_(NULL), parent_(NULL), old_available_(0), old_availablefp_(0) {}

  // This function performs the actual initialisation work.
  void Open(MacroAssembler* masm);

  // The destructor always implicitly calls the `Close` function.
  ~UseScratchRegisterScope() { Close(); }

  // This function performs the cleaning-up work. It must succeed even if the
  // scope has not been opened. It is safe to call multiple times.
  void Close();


  bool IsAvailable(const CPURegister& reg) const;


  // Take a register from the appropriate temps list. It will be returned
  // automatically when the scope ends.
  Register AcquireW() {
    return AcquireNextAvailable(masm_->GetScratchRegisterList()).W();
  }
  Register AcquireX() {
    return AcquireNextAvailable(masm_->GetScratchRegisterList()).X();
  }
  VRegister AcquireH() {
    return AcquireNextAvailable(masm_->GetScratchFPRegisterList()).H();
  }
  VRegister AcquireS() {
    return AcquireNextAvailable(masm_->GetScratchFPRegisterList()).S();
  }
  VRegister AcquireD() {
    return AcquireNextAvailable(masm_->GetScratchFPRegisterList()).D();
  }


  Register AcquireRegisterOfSize(int size_in_bits);
  Register AcquireSameSizeAs(const Register& reg) {
    return AcquireRegisterOfSize(reg.GetSizeInBits());
  }
  VRegister AcquireVRegisterOfSize(int size_in_bits);
  VRegister AcquireSameSizeAs(const VRegister& reg) {
    return AcquireVRegisterOfSize(reg.GetSizeInBits());
  }
  CPURegister AcquireCPURegisterOfSize(int size_in_bits) {
    return masm_->GetScratchRegisterList()->IsEmpty()
               ? CPURegister(AcquireVRegisterOfSize(size_in_bits))
               : CPURegister(AcquireRegisterOfSize(size_in_bits));
  }


  // Explicitly release an acquired (or excluded) register, putting it back in
  // the appropriate temps list.
  void Release(const CPURegister& reg);


  // Make the specified registers available as scratch registers for the
  // duration of this scope.
  void Include(const CPURegList& list);
  void Include(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg);
  void Include(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg);


  // Make sure that the specified registers are not available in this scope.
  // This can be used to prevent helper functions from using sensitive
  // registers, for example.
  void Exclude(const CPURegList& list);
  void Exclude(const Register& reg1,
               const Register& reg2 = NoReg,
               const Register& reg3 = NoReg,
               const Register& reg4 = NoReg);
  void Exclude(const VRegister& reg1,
               const VRegister& reg2 = NoVReg,
               const VRegister& reg3 = NoVReg,
               const VRegister& reg4 = NoVReg);
  void Exclude(const CPURegister& reg1,
               const CPURegister& reg2 = NoCPUReg,
               const CPURegister& reg3 = NoCPUReg,
               const CPURegister& reg4 = NoCPUReg);


  // Prevent any scratch registers from being used in this scope.
  void ExcludeAll();

 private:
  static CPURegister AcquireNextAvailable(CPURegList* available);

  static void ReleaseByCode(CPURegList* available, int code);

  static void ReleaseByRegList(CPURegList* available, RegList regs);

  static void IncludeByRegList(CPURegList* available, RegList exclude);

  static void ExcludeByRegList(CPURegList* available, RegList exclude);

  // The MacroAssembler maintains a list of available scratch registers, and
  // also keeps track of the most recently-opened scope so that on destruction
  // we can check that scopes do not outlive their parents.
  MacroAssembler* masm_;
  UseScratchRegisterScope* parent_;

  // The state of the available lists at the start of this scope.
  RegList old_available_;    // kRegister
  RegList old_availablefp_;  // kVRegister

  // Disallow copy constructor and operator=.
  VIXL_DEBUG_NO_RETURN UseScratchRegisterScope(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
  VIXL_DEBUG_NO_RETURN void operator=(const UseScratchRegisterScope&) {
    VIXL_UNREACHABLE();
  }
};


// Like CPUFeaturesScope, but also generate Simulation pseudo-instructions to
// control a Simulator's CPUFeatures dynamically.
//
// One major difference from CPUFeaturesScope is that this scope cannot offer
// a writable "CPUFeatures* GetCPUFeatures()", because every write to the
// features needs a corresponding macro instruction.
class SimulationCPUFeaturesScope {
 public:
  explicit SimulationCPUFeaturesScope(
      MacroAssembler* masm,
      CPUFeatures::Feature feature0 = CPUFeatures::kNone,
      CPUFeatures::Feature feature1 = CPUFeatures::kNone,
      CPUFeatures::Feature feature2 = CPUFeatures::kNone,
      CPUFeatures::Feature feature3 = CPUFeatures::kNone)
      : masm_(masm),
        cpu_features_scope_(masm, feature0, feature1, feature2, feature3) {
    masm_->SaveSimulatorCPUFeatures();
    masm_->EnableSimulatorCPUFeatures(
        CPUFeatures(feature0, feature1, feature2, feature3));
  }

  SimulationCPUFeaturesScope(MacroAssembler* masm, const CPUFeatures& other)
      : masm_(masm), cpu_features_scope_(masm, other) {
    masm_->SaveSimulatorCPUFeatures();
    masm_->EnableSimulatorCPUFeatures(other);
  }

  ~SimulationCPUFeaturesScope() { masm_->RestoreSimulatorCPUFeatures(); }

  const CPUFeatures* GetCPUFeatures() const {
    return cpu_features_scope_.GetCPUFeatures();
  }

  void SetCPUFeatures(const CPUFeatures& cpu_features) {
    cpu_features_scope_.SetCPUFeatures(cpu_features);
    masm_->SetSimulatorCPUFeatures(cpu_features);
  }

 private:
  MacroAssembler* masm_;
  CPUFeaturesScope cpu_features_scope_;
};


// Variadic templating is only available from C++11.
#ifdef VIXL_HAS_MACROASSEMBLER_RUNTIME_CALL_SUPPORT

// `R` stands for 'return type', and `P` for 'parameter types'.
template <typename R, typename... P>
void MacroAssembler::CallRuntimeHelper(R (*function)(P...),
                                       RuntimeCallType call_type) {
  if (generate_simulator_code_) {
#ifdef VIXL_HAS_SIMULATED_RUNTIME_CALL_SUPPORT
    uintptr_t runtime_call_wrapper_address = reinterpret_cast<uintptr_t>(
        &(Simulator::RuntimeCallStructHelper<R, P...>::Wrapper));
    uintptr_t function_address = reinterpret_cast<uintptr_t>(function);

    EmissionCheckScope guard(this,
                             kRuntimeCallLength,
                             CodeBufferCheckScope::kExactSize);
    Label start;
    bind(&start);
    {
      ExactAssemblyScope scope(this, kInstructionSize);
      hlt(kRuntimeCallOpcode);
    }
    VIXL_ASSERT(GetSizeOfCodeGeneratedSince(&start) ==
                kRuntimeCallWrapperOffset);
    dc(runtime_call_wrapper_address);
    VIXL_ASSERT(GetSizeOfCodeGeneratedSince(&start) ==
                kRuntimeCallFunctionOffset);
    dc(function_address);
    VIXL_ASSERT(GetSizeOfCodeGeneratedSince(&start) == kRuntimeCallTypeOffset);
    dc32(call_type);
    VIXL_ASSERT(GetSizeOfCodeGeneratedSince(&start) == kRuntimeCallLength);
#else
    VIXL_UNREACHABLE();
#endif  // #ifdef VIXL_HAS_SIMULATED_RUNTIME_CALL_SUPPORT
  } else {
    UseScratchRegisterScope temps(this);
    Register temp = temps.AcquireX();
    Mov(temp, reinterpret_cast<uint64_t>(function));
    if (call_type == kTailCallRuntime) {
      Br(temp);
    } else {
      VIXL_ASSERT(call_type == kCallRuntime);
      Blr(temp);
    }
  }
}

#endif  // #ifdef VIXL_HAS_MACROASSEMBLER_RUNTIME_CALL_SUPPORT

}  // namespace aarch64

// Required InvalSet template specialisations.
// TODO: These template specialisations should not live in this file.  Move
// VeneerPool out of the aarch64 namespace in order to share its implementation
// later.
template <>
inline ptrdiff_t InvalSet<aarch64::VeneerPool::BranchInfo,
                          aarch64::VeneerPool::kNPreallocatedInfos,
                          ptrdiff_t,
                          aarch64::VeneerPool::kInvalidOffset,
                          aarch64::VeneerPool::kReclaimFrom,
                          aarch64::VeneerPool::kReclaimFactor>::
    GetKey(const aarch64::VeneerPool::BranchInfo& branch_info) {
  return branch_info.first_unreacheable_pc_;
}
template <>
inline void InvalSet<aarch64::VeneerPool::BranchInfo,
                     aarch64::VeneerPool::kNPreallocatedInfos,
                     ptrdiff_t,
                     aarch64::VeneerPool::kInvalidOffset,
                     aarch64::VeneerPool::kReclaimFrom,
                     aarch64::VeneerPool::kReclaimFactor>::
    SetKey(aarch64::VeneerPool::BranchInfo* branch_info, ptrdiff_t key) {
  branch_info->first_unreacheable_pc_ = key;
}

}  // namespace vixl

#endif  // VIXL_AARCH64_MACRO_ASSEMBLER_AARCH64_H_
