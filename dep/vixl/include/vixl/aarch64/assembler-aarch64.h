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

#ifndef VIXL_AARCH64_ASSEMBLER_AARCH64_H_
#define VIXL_AARCH64_ASSEMBLER_AARCH64_H_

#include "../assembler-base-vixl.h"
#include "../code-generation-scopes-vixl.h"
#include "../cpu-features.h"
#include "../globals-vixl.h"
#include "../invalset-vixl.h"
#include "../utils-vixl.h"
#include "operands-aarch64.h"

namespace vixl {
namespace aarch64 {

class LabelTestHelper;  // Forward declaration.


class Label {
 public:
  Label() : location_(kLocationUnbound) {}
  ~Label() {
    // All links to a label must have been resolved before it is destructed.
    VIXL_ASSERT(!IsLinked());
  }

  bool IsBound() const { return location_ >= 0; }
  bool IsLinked() const { return !links_.empty(); }

  ptrdiff_t GetLocation() const { return location_; }
  VIXL_DEPRECATED("GetLocation", ptrdiff_t location() const) {
    return GetLocation();
  }

  static const int kNPreallocatedLinks = 4;
  static const ptrdiff_t kInvalidLinkKey = PTRDIFF_MAX;
  static const size_t kReclaimFrom = 512;
  static const size_t kReclaimFactor = 2;

  typedef InvalSet<ptrdiff_t,
                   kNPreallocatedLinks,
                   ptrdiff_t,
                   kInvalidLinkKey,
                   kReclaimFrom,
                   kReclaimFactor>
      LinksSetBase;
  typedef InvalSetIterator<LinksSetBase> LabelLinksIteratorBase;

 private:
  class LinksSet : public LinksSetBase {
   public:
    LinksSet() : LinksSetBase() {}
  };

  // Allows iterating over the links of a label. The behaviour is undefined if
  // the list of links is modified in any way while iterating.
  class LabelLinksIterator : public LabelLinksIteratorBase {
   public:
    explicit LabelLinksIterator(Label* label)
        : LabelLinksIteratorBase(&label->links_) {}

    // TODO: Remove these and use the STL-like interface instead.
    using LabelLinksIteratorBase::Advance;
    using LabelLinksIteratorBase::Current;
  };

  void Bind(ptrdiff_t location) {
    // Labels can only be bound once.
    VIXL_ASSERT(!IsBound());
    location_ = location;
  }

  void AddLink(ptrdiff_t instruction) {
    // If a label is bound, the assembler already has the information it needs
    // to write the instruction, so there is no need to add it to links_.
    VIXL_ASSERT(!IsBound());
    links_.insert(instruction);
  }

  void DeleteLink(ptrdiff_t instruction) { links_.erase(instruction); }

  void ClearAllLinks() { links_.clear(); }

  // TODO: The comment below considers average case complexity for our
  // usual use-cases. The elements of interest are:
  // - Branches to a label are emitted in order: branch instructions to a label
  // are generated at an offset in the code generation buffer greater than any
  // other branch to that same label already generated. As an example, this can
  // be broken when an instruction is patched to become a branch. Note that the
  // code will still work, but the complexity considerations below may locally
  // not apply any more.
  // - Veneers are generated in order: for multiple branches of the same type
  // branching to the same unbound label going out of range, veneers are
  // generated in growing order of the branch instruction offset from the start
  // of the buffer.
  //
  // When creating a veneer for a branch going out of range, the link for this
  // branch needs to be removed from this `links_`. Since all branches are
  // tracked in one underlying InvalSet, the complexity for this deletion is the
  // same as for finding the element, ie. O(n), where n is the number of links
  // in the set.
  // This could be reduced to O(1) by using the same trick as used when tracking
  // branch information for veneers: split the container to use one set per type
  // of branch. With that setup, when a veneer is created and the link needs to
  // be deleted, if the two points above hold, it must be the minimum element of
  // the set for its type of branch, and that minimum element will be accessible
  // in O(1).

  // The offsets of the instructions that have linked to this label.
  LinksSet links_;
  // The label location.
  ptrdiff_t location_;

  static const ptrdiff_t kLocationUnbound = -1;

// It is not safe to copy labels, so disable the copy constructor and operator
// by declaring them private (without an implementation).
#if __cplusplus >= 201103L
  Label(const Label&) = delete;
  void operator=(const Label&) = delete;
#else
  Label(const Label&);
  void operator=(const Label&);
#endif

  // The Assembler class is responsible for binding and linking labels, since
  // the stored offsets need to be consistent with the Assembler's buffer.
  friend class Assembler;
  // The MacroAssembler and VeneerPool handle resolution of branches to distant
  // targets.
  friend class MacroAssembler;
  friend class VeneerPool;
};


class Assembler;
class LiteralPool;

// A literal is a 32-bit or 64-bit piece of data stored in the instruction
// stream and loaded through a pc relative load. The same literal can be
// referred to by multiple instructions but a literal can only reside at one
// place in memory. A literal can be used by a load before or after being
// placed in memory.
//
// Internally an offset of 0 is associated with a literal which has been
// neither used nor placed. Then two possibilities arise:
//  1) the label is placed, the offset (stored as offset + 1) is used to
//     resolve any subsequent load using the label.
//  2) the label is not placed and offset is the offset of the last load using
//     the literal (stored as -offset -1). If multiple loads refer to this
//     literal then the last load holds the offset of the preceding load and
//     all loads form a chain. Once the offset is placed all the loads in the
//     chain are resolved and future loads fall back to possibility 1.
class RawLiteral {
 public:
  enum DeletionPolicy {
    kDeletedOnPlacementByPool,
    kDeletedOnPoolDestruction,
    kManuallyDeleted
  };

  RawLiteral(size_t size,
             LiteralPool* literal_pool,
             DeletionPolicy deletion_policy = kManuallyDeleted);

  // The literal pool only sees and deletes `RawLiteral*` pointers, but they are
  // actually pointing to `Literal<T>` objects.
  virtual ~RawLiteral() {}

  size_t GetSize() const {
    VIXL_STATIC_ASSERT(kDRegSizeInBytes == kXRegSizeInBytes);
    VIXL_STATIC_ASSERT(kSRegSizeInBytes == kWRegSizeInBytes);
    VIXL_ASSERT((size_ == kXRegSizeInBytes) || (size_ == kWRegSizeInBytes) ||
                (size_ == kQRegSizeInBytes));
    return size_;
  }
  VIXL_DEPRECATED("GetSize", size_t size()) { return GetSize(); }

  uint64_t GetRawValue128Low64() const {
    VIXL_ASSERT(size_ == kQRegSizeInBytes);
    return low64_;
  }
  VIXL_DEPRECATED("GetRawValue128Low64", uint64_t raw_value128_low64()) {
    return GetRawValue128Low64();
  }

  uint64_t GetRawValue128High64() const {
    VIXL_ASSERT(size_ == kQRegSizeInBytes);
    return high64_;
  }
  VIXL_DEPRECATED("GetRawValue128High64", uint64_t raw_value128_high64()) {
    return GetRawValue128High64();
  }

  uint64_t GetRawValue64() const {
    VIXL_ASSERT(size_ == kXRegSizeInBytes);
    VIXL_ASSERT(high64_ == 0);
    return low64_;
  }
  VIXL_DEPRECATED("GetRawValue64", uint64_t raw_value64()) {
    return GetRawValue64();
  }

  uint32_t GetRawValue32() const {
    VIXL_ASSERT(size_ == kWRegSizeInBytes);
    VIXL_ASSERT(high64_ == 0);
    VIXL_ASSERT(IsUint32(low64_) || IsInt32(low64_));
    return static_cast<uint32_t>(low64_);
  }
  VIXL_DEPRECATED("GetRawValue32", uint32_t raw_value32()) {
    return GetRawValue32();
  }

  bool IsUsed() const { return offset_ < 0; }
  bool IsPlaced() const { return offset_ > 0; }

  LiteralPool* GetLiteralPool() const { return literal_pool_; }

  ptrdiff_t GetOffset() const {
    VIXL_ASSERT(IsPlaced());
    return offset_ - 1;
  }
  VIXL_DEPRECATED("GetOffset", ptrdiff_t offset()) { return GetOffset(); }

 protected:
  void SetOffset(ptrdiff_t offset) {
    VIXL_ASSERT(offset >= 0);
    VIXL_ASSERT(IsWordAligned(offset));
    VIXL_ASSERT(!IsPlaced());
    offset_ = offset + 1;
  }
  VIXL_DEPRECATED("SetOffset", void set_offset(ptrdiff_t offset)) {
    SetOffset(offset);
  }

  ptrdiff_t GetLastUse() const {
    VIXL_ASSERT(IsUsed());
    return -offset_ - 1;
  }
  VIXL_DEPRECATED("GetLastUse", ptrdiff_t last_use()) { return GetLastUse(); }

  void SetLastUse(ptrdiff_t offset) {
    VIXL_ASSERT(offset >= 0);
    VIXL_ASSERT(IsWordAligned(offset));
    VIXL_ASSERT(!IsPlaced());
    offset_ = -offset - 1;
  }
  VIXL_DEPRECATED("SetLastUse", void set_last_use(ptrdiff_t offset)) {
    SetLastUse(offset);
  }

  size_t size_;
  ptrdiff_t offset_;
  uint64_t low64_;
  uint64_t high64_;

 private:
  LiteralPool* literal_pool_;
  DeletionPolicy deletion_policy_;

  friend class Assembler;
  friend class LiteralPool;
};


template <typename T>
class Literal : public RawLiteral {
 public:
  explicit Literal(T value,
                   LiteralPool* literal_pool = NULL,
                   RawLiteral::DeletionPolicy ownership = kManuallyDeleted)
      : RawLiteral(sizeof(value), literal_pool, ownership) {
    VIXL_STATIC_ASSERT(sizeof(value) <= kXRegSizeInBytes);
    UpdateValue(value);
  }

  Literal(T high64,
          T low64,
          LiteralPool* literal_pool = NULL,
          RawLiteral::DeletionPolicy ownership = kManuallyDeleted)
      : RawLiteral(kQRegSizeInBytes, literal_pool, ownership) {
    VIXL_STATIC_ASSERT(sizeof(low64) == (kQRegSizeInBytes / 2));
    UpdateValue(high64, low64);
  }

  virtual ~Literal() {}

  // Update the value of this literal, if necessary by rewriting the value in
  // the pool.
  // If the literal has already been placed in a literal pool, the address of
  // the start of the code buffer must be provided, as the literal only knows it
  // offset from there. This also allows patching the value after the code has
  // been moved in memory.
  void UpdateValue(T new_value, uint8_t* code_buffer = NULL) {
    VIXL_ASSERT(sizeof(new_value) == size_);
    memcpy(&low64_, &new_value, sizeof(new_value));
    if (IsPlaced()) {
      VIXL_ASSERT(code_buffer != NULL);
      RewriteValueInCode(code_buffer);
    }
  }

  void UpdateValue(T high64, T low64, uint8_t* code_buffer = NULL) {
    VIXL_ASSERT(sizeof(low64) == size_ / 2);
    memcpy(&low64_, &low64, sizeof(low64));
    memcpy(&high64_, &high64, sizeof(high64));
    if (IsPlaced()) {
      VIXL_ASSERT(code_buffer != NULL);
      RewriteValueInCode(code_buffer);
    }
  }

  void UpdateValue(T new_value, const Assembler* assembler);
  void UpdateValue(T high64, T low64, const Assembler* assembler);

 private:
  void RewriteValueInCode(uint8_t* code_buffer) {
    VIXL_ASSERT(IsPlaced());
    VIXL_STATIC_ASSERT(sizeof(T) <= kXRegSizeInBytes);
    switch (GetSize()) {
      case kSRegSizeInBytes:
        *reinterpret_cast<uint32_t*>(code_buffer + GetOffset()) =
            GetRawValue32();
        break;
      case kDRegSizeInBytes:
        *reinterpret_cast<uint64_t*>(code_buffer + GetOffset()) =
            GetRawValue64();
        break;
      default:
        VIXL_ASSERT(GetSize() == kQRegSizeInBytes);
        uint64_t* base_address =
            reinterpret_cast<uint64_t*>(code_buffer + GetOffset());
        *base_address = GetRawValue128Low64();
        *(base_address + 1) = GetRawValue128High64();
    }
  }
};


// Control whether or not position-independent code should be emitted.
enum PositionIndependentCodeOption {
  // All code generated will be position-independent; all branches and
  // references to labels generated with the Label class will use PC-relative
  // addressing.
  PositionIndependentCode,

  // Allow VIXL to generate code that refers to absolute addresses. With this
  // option, it will not be possible to copy the code buffer and run it from a
  // different address; code must be generated in its final location.
  PositionDependentCode,

  // Allow VIXL to assume that the bottom 12 bits of the address will be
  // constant, but that the top 48 bits may change. This allows `adrp` to
  // function in systems which copy code between pages, but otherwise maintain
  // 4KB page alignment.
  PageOffsetDependentCode
};


// Control how scaled- and unscaled-offset loads and stores are generated.
enum LoadStoreScalingOption {
  // Prefer scaled-immediate-offset instructions, but emit unscaled-offset,
  // register-offset, pre-index or post-index instructions if necessary.
  PreferScaledOffset,

  // Prefer unscaled-immediate-offset instructions, but emit scaled-offset,
  // register-offset, pre-index or post-index instructions if necessary.
  PreferUnscaledOffset,

  // Require scaled-immediate-offset instructions.
  RequireScaledOffset,

  // Require unscaled-immediate-offset instructions.
  RequireUnscaledOffset
};


// Assembler.
class Assembler : public vixl::internal::AssemblerBase {
 public:
  explicit Assembler(
      PositionIndependentCodeOption pic = PositionIndependentCode)
      : pic_(pic), cpu_features_(CPUFeatures::AArch64LegacyBaseline()) {}
  explicit Assembler(
      size_t capacity,
      PositionIndependentCodeOption pic = PositionIndependentCode)
      : AssemblerBase(capacity),
        pic_(pic),
        cpu_features_(CPUFeatures::AArch64LegacyBaseline()) {}
  Assembler(byte* buffer,
            size_t capacity,
            PositionIndependentCodeOption pic = PositionIndependentCode)
      : AssemblerBase(buffer, capacity),
        pic_(pic),
        cpu_features_(CPUFeatures::AArch64LegacyBaseline()) {}

  // Upon destruction, the code will assert that one of the following is true:
  //  * The Assembler object has not been used.
  //  * Nothing has been emitted since the last Reset() call.
  //  * Nothing has been emitted since the last FinalizeCode() call.
  ~Assembler() {}

  // System functions.

  // Start generating code from the beginning of the buffer, discarding any code
  // and data that has already been emitted into the buffer.
  void Reset();

  // Label.
  // Bind a label to the current PC.
  void bind(Label* label);

  // Bind a label to a specified offset from the start of the buffer.
  void BindToOffset(Label* label, ptrdiff_t offset);

  // Place a literal at the current PC.
  void place(RawLiteral* literal);

  VIXL_DEPRECATED("GetCursorOffset", ptrdiff_t CursorOffset() const) {
    return GetCursorOffset();
  }

  VIXL_DEPRECATED("GetBuffer().GetCapacity()",
                  ptrdiff_t GetBufferEndOffset() const) {
    return static_cast<ptrdiff_t>(GetBuffer().GetCapacity());
  }
  VIXL_DEPRECATED("GetBuffer().GetCapacity()",
                  ptrdiff_t BufferEndOffset() const) {
    return GetBuffer().GetCapacity();
  }

  // Return the address of a bound label.
  template <typename T>
  T GetLabelAddress(const Label* label) const {
    VIXL_ASSERT(label->IsBound());
    VIXL_STATIC_ASSERT(sizeof(T) >= sizeof(uintptr_t));
    return GetBuffer().GetOffsetAddress<T>(label->GetLocation());
  }

  Instruction* GetInstructionAt(ptrdiff_t instruction_offset) {
    return GetBuffer()->GetOffsetAddress<Instruction*>(instruction_offset);
  }
  VIXL_DEPRECATED("GetInstructionAt",
                  Instruction* InstructionAt(ptrdiff_t instruction_offset)) {
    return GetInstructionAt(instruction_offset);
  }

  ptrdiff_t GetInstructionOffset(Instruction* instruction) {
    VIXL_STATIC_ASSERT(sizeof(*instruction) == 1);
    ptrdiff_t offset =
        instruction - GetBuffer()->GetStartAddress<Instruction*>();
    VIXL_ASSERT((0 <= offset) &&
                (offset < static_cast<ptrdiff_t>(GetBuffer()->GetCapacity())));
    return offset;
  }
  VIXL_DEPRECATED("GetInstructionOffset",
                  ptrdiff_t InstructionOffset(Instruction* instruction)) {
    return GetInstructionOffset(instruction);
  }

  // Instruction set functions.

  // Branch / Jump instructions.
  // Branch to register.
  void br(const Register& xn);

  // Branch with link to register.
  void blr(const Register& xn);

  // Branch to register with return hint.
  void ret(const Register& xn = lr);

  // Branch to register, with pointer authentication. Using key A and a modifier
  // of zero [Armv8.3].
  void braaz(const Register& xn);

  // Branch to register, with pointer authentication. Using key B and a modifier
  // of zero [Armv8.3].
  void brabz(const Register& xn);

  // Branch with link to register, with pointer authentication. Using key A and
  // a modifier of zero [Armv8.3].
  void blraaz(const Register& xn);

  // Branch with link to register, with pointer authentication. Using key B and
  // a modifier of zero [Armv8.3].
  void blrabz(const Register& xn);

  // Return from subroutine, with pointer authentication. Using key A [Armv8.3].
  void retaa();

  // Return from subroutine, with pointer authentication. Using key B [Armv8.3].
  void retab();

  // Branch to register, with pointer authentication. Using key A [Armv8.3].
  void braa(const Register& xn, const Register& xm);

  // Branch to register, with pointer authentication. Using key B [Armv8.3].
  void brab(const Register& xn, const Register& xm);

  // Branch with link to register, with pointer authentication. Using key A
  // [Armv8.3].
  void blraa(const Register& xn, const Register& xm);

  // Branch with link to register, with pointer authentication. Using key B
  // [Armv8.3].
  void blrab(const Register& xn, const Register& xm);

  // Unconditional branch to label.
  void b(Label* label);

  // Conditional branch to label.
  void b(Label* label, Condition cond);

  // Unconditional branch to PC offset.
  void b(int64_t imm26);

  // Conditional branch to PC offset.
  void b(int64_t imm19, Condition cond);

  // Branch with link to label.
  void bl(Label* label);

  // Branch with link to PC offset.
  void bl(int64_t imm26);

  // Compare and branch to label if zero.
  void cbz(const Register& rt, Label* label);

  // Compare and branch to PC offset if zero.
  void cbz(const Register& rt, int64_t imm19);

  // Compare and branch to label if not zero.
  void cbnz(const Register& rt, Label* label);

  // Compare and branch to PC offset if not zero.
  void cbnz(const Register& rt, int64_t imm19);

  // Table lookup from one register.
  void tbl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Table lookup from two registers.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm);

  // Table lookup from three registers.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm);

  // Table lookup from four registers.
  void tbl(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm);

  // Table lookup extension from one register.
  void tbx(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Table lookup extension from two registers.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vm);

  // Table lookup extension from three registers.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vm);

  // Table lookup extension from four registers.
  void tbx(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vn2,
           const VRegister& vn3,
           const VRegister& vn4,
           const VRegister& vm);

  // Test bit and branch to label if zero.
  void tbz(const Register& rt, unsigned bit_pos, Label* label);

  // Test bit and branch to PC offset if zero.
  void tbz(const Register& rt, unsigned bit_pos, int64_t imm14);

  // Test bit and branch to label if not zero.
  void tbnz(const Register& rt, unsigned bit_pos, Label* label);

  // Test bit and branch to PC offset if not zero.
  void tbnz(const Register& rt, unsigned bit_pos, int64_t imm14);

  // Address calculation instructions.
  // Calculate a PC-relative address. Unlike for branches the offset in adr is
  // unscaled (i.e. the result can be unaligned).

  // Calculate the address of a label.
  void adr(const Register& xd, Label* label);

  // Calculate the address of a PC offset.
  void adr(const Register& xd, int64_t imm21);

  // Calculate the page address of a label.
  void adrp(const Register& xd, Label* label);

  // Calculate the page address of a PC offset.
  void adrp(const Register& xd, int64_t imm21);

  // Data Processing instructions.
  // Add.
  void add(const Register& rd, const Register& rn, const Operand& operand);

  // Add and update status flags.
  void adds(const Register& rd, const Register& rn, const Operand& operand);

  // Compare negative.
  void cmn(const Register& rn, const Operand& operand);

  // Subtract.
  void sub(const Register& rd, const Register& rn, const Operand& operand);

  // Subtract and update status flags.
  void subs(const Register& rd, const Register& rn, const Operand& operand);

  // Compare.
  void cmp(const Register& rn, const Operand& operand);

  // Negate.
  void neg(const Register& rd, const Operand& operand);

  // Negate and update status flags.
  void negs(const Register& rd, const Operand& operand);

  // Add with carry bit.
  void adc(const Register& rd, const Register& rn, const Operand& operand);

  // Add with carry bit and update status flags.
  void adcs(const Register& rd, const Register& rn, const Operand& operand);

  // Subtract with carry bit.
  void sbc(const Register& rd, const Register& rn, const Operand& operand);

  // Subtract with carry bit and update status flags.
  void sbcs(const Register& rd, const Register& rn, const Operand& operand);

  // Negate with carry bit.
  void ngc(const Register& rd, const Operand& operand);

  // Negate with carry bit and update status flags.
  void ngcs(const Register& rd, const Operand& operand);

  // Logical instructions.
  // Bitwise and (A & B).
  void and_(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise and (A & B) and update status flags.
  void ands(const Register& rd, const Register& rn, const Operand& operand);

  // Bit test and set flags.
  void tst(const Register& rn, const Operand& operand);

  // Bit clear (A & ~B).
  void bic(const Register& rd, const Register& rn, const Operand& operand);

  // Bit clear (A & ~B) and update status flags.
  void bics(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise or (A | B).
  void orr(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise nor (A | ~B).
  void orn(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise eor/xor (A ^ B).
  void eor(const Register& rd, const Register& rn, const Operand& operand);

  // Bitwise enor/xnor (A ^ ~B).
  void eon(const Register& rd, const Register& rn, const Operand& operand);

  // Logical shift left by variable.
  void lslv(const Register& rd, const Register& rn, const Register& rm);

  // Logical shift right by variable.
  void lsrv(const Register& rd, const Register& rn, const Register& rm);

  // Arithmetic shift right by variable.
  void asrv(const Register& rd, const Register& rn, const Register& rm);

  // Rotate right by variable.
  void rorv(const Register& rd, const Register& rn, const Register& rm);

  // Bitfield instructions.
  // Bitfield move.
  void bfm(const Register& rd,
           const Register& rn,
           unsigned immr,
           unsigned imms);

  // Signed bitfield move.
  void sbfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms);

  // Unsigned bitfield move.
  void ubfm(const Register& rd,
            const Register& rn,
            unsigned immr,
            unsigned imms);

  // Bfm aliases.
  // Bitfield insert.
  void bfi(const Register& rd,
           const Register& rn,
           unsigned lsb,
           unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= static_cast<unsigned>(rn.GetSizeInBits()));
    bfm(rd,
        rn,
        (rd.GetSizeInBits() - lsb) & (rd.GetSizeInBits() - 1),
        width - 1);
  }

  // Bitfield extract and insert low.
  void bfxil(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= static_cast<unsigned>(rn.GetSizeInBits()));
    bfm(rd, rn, lsb, lsb + width - 1);
  }

  // Bitfield clear [Armv8.2].
  void bfc(const Register& rd, unsigned lsb, unsigned width) {
    bfi(rd, AppropriateZeroRegFor(rd), lsb, width);
  }

  // Sbfm aliases.
  // Arithmetic shift right.
  void asr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(shift < static_cast<unsigned>(rd.GetSizeInBits()));
    sbfm(rd, rn, shift, rd.GetSizeInBits() - 1);
  }

  // Signed bitfield insert with zero at right.
  void sbfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= static_cast<unsigned>(rn.GetSizeInBits()));
    sbfm(rd,
         rn,
         (rd.GetSizeInBits() - lsb) & (rd.GetSizeInBits() - 1),
         width - 1);
  }

  // Signed bitfield extract.
  void sbfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= static_cast<unsigned>(rn.GetSizeInBits()));
    sbfm(rd, rn, lsb, lsb + width - 1);
  }

  // Signed extend byte.
  void sxtb(const Register& rd, const Register& rn) { sbfm(rd, rn, 0, 7); }

  // Signed extend halfword.
  void sxth(const Register& rd, const Register& rn) { sbfm(rd, rn, 0, 15); }

  // Signed extend word.
  void sxtw(const Register& rd, const Register& rn) { sbfm(rd, rn, 0, 31); }

  // Ubfm aliases.
  // Logical shift left.
  void lsl(const Register& rd, const Register& rn, unsigned shift) {
    unsigned reg_size = rd.GetSizeInBits();
    VIXL_ASSERT(shift < reg_size);
    ubfm(rd, rn, (reg_size - shift) % reg_size, reg_size - shift - 1);
  }

  // Logical shift right.
  void lsr(const Register& rd, const Register& rn, unsigned shift) {
    VIXL_ASSERT(shift < static_cast<unsigned>(rd.GetSizeInBits()));
    ubfm(rd, rn, shift, rd.GetSizeInBits() - 1);
  }

  // Unsigned bitfield insert with zero at right.
  void ubfiz(const Register& rd,
             const Register& rn,
             unsigned lsb,
             unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= static_cast<unsigned>(rn.GetSizeInBits()));
    ubfm(rd,
         rn,
         (rd.GetSizeInBits() - lsb) & (rd.GetSizeInBits() - 1),
         width - 1);
  }

  // Unsigned bitfield extract.
  void ubfx(const Register& rd,
            const Register& rn,
            unsigned lsb,
            unsigned width) {
    VIXL_ASSERT(width >= 1);
    VIXL_ASSERT(lsb + width <= static_cast<unsigned>(rn.GetSizeInBits()));
    ubfm(rd, rn, lsb, lsb + width - 1);
  }

  // Unsigned extend byte.
  void uxtb(const Register& rd, const Register& rn) { ubfm(rd, rn, 0, 7); }

  // Unsigned extend halfword.
  void uxth(const Register& rd, const Register& rn) { ubfm(rd, rn, 0, 15); }

  // Unsigned extend word.
  void uxtw(const Register& rd, const Register& rn) { ubfm(rd, rn, 0, 31); }

  // Extract.
  void extr(const Register& rd,
            const Register& rn,
            const Register& rm,
            unsigned lsb);

  // Conditional select: rd = cond ? rn : rm.
  void csel(const Register& rd,
            const Register& rn,
            const Register& rm,
            Condition cond);

  // Conditional select increment: rd = cond ? rn : rm + 1.
  void csinc(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond);

  // Conditional select inversion: rd = cond ? rn : ~rm.
  void csinv(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond);

  // Conditional select negation: rd = cond ? rn : -rm.
  void csneg(const Register& rd,
             const Register& rn,
             const Register& rm,
             Condition cond);

  // Conditional set: rd = cond ? 1 : 0.
  void cset(const Register& rd, Condition cond);

  // Conditional set mask: rd = cond ? -1 : 0.
  void csetm(const Register& rd, Condition cond);

  // Conditional increment: rd = cond ? rn + 1 : rn.
  void cinc(const Register& rd, const Register& rn, Condition cond);

  // Conditional invert: rd = cond ? ~rn : rn.
  void cinv(const Register& rd, const Register& rn, Condition cond);

  // Conditional negate: rd = cond ? -rn : rn.
  void cneg(const Register& rd, const Register& rn, Condition cond);

  // Rotate right.
  void ror(const Register& rd, const Register& rs, unsigned shift) {
    extr(rd, rs, rs, shift);
  }

  // Conditional comparison.
  // Conditional compare negative.
  void ccmn(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);

  // Conditional compare.
  void ccmp(const Register& rn,
            const Operand& operand,
            StatusFlags nzcv,
            Condition cond);

  // CRC-32 checksum from byte.
  void crc32b(const Register& wd, const Register& wn, const Register& wm);

  // CRC-32 checksum from half-word.
  void crc32h(const Register& wd, const Register& wn, const Register& wm);

  // CRC-32 checksum from word.
  void crc32w(const Register& wd, const Register& wn, const Register& wm);

  // CRC-32 checksum from double word.
  void crc32x(const Register& wd, const Register& wn, const Register& xm);

  // CRC-32 C checksum from byte.
  void crc32cb(const Register& wd, const Register& wn, const Register& wm);

  // CRC-32 C checksum from half-word.
  void crc32ch(const Register& wd, const Register& wn, const Register& wm);

  // CRC-32 C checksum from word.
  void crc32cw(const Register& wd, const Register& wn, const Register& wm);

  // CRC-32C checksum from double word.
  void crc32cx(const Register& wd, const Register& wn, const Register& xm);

  // Multiply.
  void mul(const Register& rd, const Register& rn, const Register& rm);

  // Negated multiply.
  void mneg(const Register& rd, const Register& rn, const Register& rm);

  // Signed long multiply: 32 x 32 -> 64-bit.
  void smull(const Register& xd, const Register& wn, const Register& wm);

  // Signed multiply high: 64 x 64 -> 64-bit <127:64>.
  void smulh(const Register& xd, const Register& xn, const Register& xm);

  // Multiply and accumulate.
  void madd(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra);

  // Multiply and subtract.
  void msub(const Register& rd,
            const Register& rn,
            const Register& rm,
            const Register& ra);

  // Signed long multiply and accumulate: 32 x 32 + 64 -> 64-bit.
  void smaddl(const Register& xd,
              const Register& wn,
              const Register& wm,
              const Register& xa);

  // Unsigned long multiply and accumulate: 32 x 32 + 64 -> 64-bit.
  void umaddl(const Register& xd,
              const Register& wn,
              const Register& wm,
              const Register& xa);

  // Unsigned long multiply: 32 x 32 -> 64-bit.
  void umull(const Register& xd, const Register& wn, const Register& wm) {
    umaddl(xd, wn, wm, xzr);
  }

  // Unsigned multiply high: 64 x 64 -> 64-bit <127:64>.
  void umulh(const Register& xd, const Register& xn, const Register& xm);

  // Signed long multiply and subtract: 64 - (32 x 32) -> 64-bit.
  void smsubl(const Register& xd,
              const Register& wn,
              const Register& wm,
              const Register& xa);

  // Unsigned long multiply and subtract: 64 - (32 x 32) -> 64-bit.
  void umsubl(const Register& xd,
              const Register& wn,
              const Register& wm,
              const Register& xa);

  // Signed integer divide.
  void sdiv(const Register& rd, const Register& rn, const Register& rm);

  // Unsigned integer divide.
  void udiv(const Register& rd, const Register& rn, const Register& rm);

  // Bit reverse.
  void rbit(const Register& rd, const Register& rn);

  // Reverse bytes in 16-bit half words.
  void rev16(const Register& rd, const Register& rn);

  // Reverse bytes in 32-bit words.
  void rev32(const Register& xd, const Register& xn);

  // Reverse bytes in 64-bit general purpose register, an alias for rev
  // [Armv8.2].
  void rev64(const Register& xd, const Register& xn) {
    VIXL_ASSERT(xd.Is64Bits() && xn.Is64Bits());
    rev(xd, xn);
  }

  // Reverse bytes.
  void rev(const Register& rd, const Register& rn);

  // Count leading zeroes.
  void clz(const Register& rd, const Register& rn);

  // Count leading sign bits.
  void cls(const Register& rd, const Register& rn);

  // Pointer Authentication Code for Instruction address, using key A [Armv8.3].
  void pacia(const Register& xd, const Register& rn);

  // Pointer Authentication Code for Instruction address, using key A and a
  // modifier of zero [Armv8.3].
  void paciza(const Register& xd);

  // Pointer Authentication Code for Instruction address, using key A, with
  // address in x17 and modifier in x16 [Armv8.3].
  void pacia1716();

  // Pointer Authentication Code for Instruction address, using key A, with
  // address in LR and modifier in SP [Armv8.3].
  void paciasp();

  // Pointer Authentication Code for Instruction address, using key A, with
  // address in LR and a modifier of zero [Armv8.3].
  void paciaz();

  // Pointer Authentication Code for Instruction address, using key B [Armv8.3].
  void pacib(const Register& xd, const Register& xn);

  // Pointer Authentication Code for Instruction address, using key B and a
  // modifier of zero [Armv8.3].
  void pacizb(const Register& xd);

  // Pointer Authentication Code for Instruction address, using key B, with
  // address in x17 and modifier in x16 [Armv8.3].
  void pacib1716();

  // Pointer Authentication Code for Instruction address, using key B, with
  // address in LR and modifier in SP [Armv8.3].
  void pacibsp();

  // Pointer Authentication Code for Instruction address, using key B, with
  // address in LR and a modifier of zero [Armv8.3].
  void pacibz();

  // Pointer Authentication Code for Data address, using key A [Armv8.3].
  void pacda(const Register& xd, const Register& xn);

  // Pointer Authentication Code for Data address, using key A and a modifier of
  // zero [Armv8.3].
  void pacdza(const Register& xd);

  // Pointer Authentication Code for Data address, using key A, with address in
  // x17 and modifier in x16 [Armv8.3].
  void pacda1716();

  // Pointer Authentication Code for Data address, using key A, with address in
  // LR and modifier in SP [Armv8.3].
  void pacdasp();

  // Pointer Authentication Code for Data address, using key A, with address in
  // LR and a modifier of zero [Armv8.3].
  void pacdaz();

  // Pointer Authentication Code for Data address, using key B [Armv8.3].
  void pacdb(const Register& xd, const Register& xn);

  // Pointer Authentication Code for Data address, using key B and a modifier of
  // zero [Armv8.3].
  void pacdzb(const Register& xd);

  // Pointer Authentication Code for Data address, using key B, with address in
  // x17 and modifier in x16 [Armv8.3].
  void pacdb1716();

  // Pointer Authentication Code for Data address, using key B, with address in
  // LR and modifier in SP [Armv8.3].
  void pacdbsp();

  // Pointer Authentication Code for Data address, using key B, with address in
  // LR and a modifier of zero [Armv8.3].
  void pacdbz();

  // Pointer Authentication Code, using Generic key [Armv8.3].
  void pacga(const Register& xd, const Register& xn, const Register& xm);

  // Authenticate Instruction address, using key A [Armv8.3].
  void autia(const Register& xd, const Register& xn);

  // Authenticate Instruction address, using key A and a modifier of zero
  // [Armv8.3].
  void autiza(const Register& xd);

  // Authenticate Instruction address, using key A, with address in x17 and
  // modifier in x16 [Armv8.3].
  void autia1716();

  // Authenticate Instruction address, using key A, with address in LR and
  // modifier in SP [Armv8.3].
  void autiasp();

  // Authenticate Instruction address, using key A, with address in LR and a
  // modifier of zero [Armv8.3].
  void autiaz();

  // Authenticate Instruction address, using key B [Armv8.3].
  void autib(const Register& xd, const Register& xn);

  // Authenticate Instruction address, using key B and a modifier of zero
  // [Armv8.3].
  void autizb(const Register& xd);

  // Authenticate Instruction address, using key B, with address in x17 and
  // modifier in x16 [Armv8.3].
  void autib1716();

  // Authenticate Instruction address, using key B, with address in LR and
  // modifier in SP [Armv8.3].
  void autibsp();

  // Authenticate Instruction address, using key B, with address in LR and a
  // modifier of zero [Armv8.3].
  void autibz();

  // Authenticate Data address, using key A [Armv8.3].
  void autda(const Register& xd, const Register& xn);

  // Authenticate Data address, using key A and a modifier of zero [Armv8.3].
  void autdza(const Register& xd);

  // Authenticate Data address, using key A, with address in x17 and modifier in
  // x16 [Armv8.3].
  void autda1716();

  // Authenticate Data address, using key A, with address in LR and modifier in
  // SP [Armv8.3].
  void autdasp();

  // Authenticate Data address, using key A, with address in LR and a modifier
  // of zero [Armv8.3].
  void autdaz();

  // Authenticate Data address, using key B [Armv8.3].
  void autdb(const Register& xd, const Register& xn);

  // Authenticate Data address, using key B and a modifier of zero [Armv8.3].
  void autdzb(const Register& xd);

  // Authenticate Data address, using key B, with address in x17 and modifier in
  // x16 [Armv8.3].
  void autdb1716();

  // Authenticate Data address, using key B, with address in LR and modifier in
  // SP [Armv8.3].
  void autdbsp();

  // Authenticate Data address, using key B, with address in LR and a modifier
  // of zero [Armv8.3].
  void autdbz();

  // Strip Pointer Authentication Code of Data address [Armv8.3].
  void xpacd(const Register& xd);

  // Strip Pointer Authentication Code of Instruction address [Armv8.3].
  void xpaci(const Register& xd);

  // Strip Pointer Authentication Code of Instruction address in LR [Armv8.3].
  void xpaclri();

  // Memory instructions.
  // Load integer or FP register.
  void ldr(const CPURegister& rt,
           const MemOperand& src,
           LoadStoreScalingOption option = PreferScaledOffset);

  // Store integer or FP register.
  void str(const CPURegister& rt,
           const MemOperand& dst,
           LoadStoreScalingOption option = PreferScaledOffset);

  // Load word with sign extension.
  void ldrsw(const Register& xt,
             const MemOperand& src,
             LoadStoreScalingOption option = PreferScaledOffset);

  // Load byte.
  void ldrb(const Register& rt,
            const MemOperand& src,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Store byte.
  void strb(const Register& rt,
            const MemOperand& dst,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Load byte with sign extension.
  void ldrsb(const Register& rt,
             const MemOperand& src,
             LoadStoreScalingOption option = PreferScaledOffset);

  // Load half-word.
  void ldrh(const Register& rt,
            const MemOperand& src,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Store half-word.
  void strh(const Register& rt,
            const MemOperand& dst,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Load half-word with sign extension.
  void ldrsh(const Register& rt,
             const MemOperand& src,
             LoadStoreScalingOption option = PreferScaledOffset);

  // Load integer or FP register (with unscaled offset).
  void ldur(const CPURegister& rt,
            const MemOperand& src,
            LoadStoreScalingOption option = PreferUnscaledOffset);

  // Store integer or FP register (with unscaled offset).
  void stur(const CPURegister& rt,
            const MemOperand& src,
            LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load word with sign extension.
  void ldursw(const Register& xt,
              const MemOperand& src,
              LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load byte (with unscaled offset).
  void ldurb(const Register& rt,
             const MemOperand& src,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Store byte (with unscaled offset).
  void sturb(const Register& rt,
             const MemOperand& dst,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load byte with sign extension (and unscaled offset).
  void ldursb(const Register& rt,
              const MemOperand& src,
              LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load half-word (with unscaled offset).
  void ldurh(const Register& rt,
             const MemOperand& src,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Store half-word (with unscaled offset).
  void sturh(const Register& rt,
             const MemOperand& dst,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load half-word with sign extension (and unscaled offset).
  void ldursh(const Register& rt,
              const MemOperand& src,
              LoadStoreScalingOption option = PreferUnscaledOffset);

  // Load integer or FP register pair.
  void ldp(const CPURegister& rt,
           const CPURegister& rt2,
           const MemOperand& src);

  // Store integer or FP register pair.
  void stp(const CPURegister& rt,
           const CPURegister& rt2,
           const MemOperand& dst);

  // Load word pair with sign extension.
  void ldpsw(const Register& xt, const Register& xt2, const MemOperand& src);

  // Load integer or FP register pair, non-temporal.
  void ldnp(const CPURegister& rt,
            const CPURegister& rt2,
            const MemOperand& src);

  // Store integer or FP register pair, non-temporal.
  void stnp(const CPURegister& rt,
            const CPURegister& rt2,
            const MemOperand& dst);

  // Load integer or FP register from literal pool.
  void ldr(const CPURegister& rt, RawLiteral* literal);

  // Load word with sign extension from literal pool.
  void ldrsw(const Register& xt, RawLiteral* literal);

  // Load integer or FP register from pc + imm19 << 2.
  void ldr(const CPURegister& rt, int64_t imm19);

  // Load word with sign extension from pc + imm19 << 2.
  void ldrsw(const Register& xt, int64_t imm19);

  // Store exclusive byte.
  void stxrb(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store exclusive half-word.
  void stxrh(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store exclusive register.
  void stxr(const Register& rs, const Register& rt, const MemOperand& dst);

  // Load exclusive byte.
  void ldxrb(const Register& rt, const MemOperand& src);

  // Load exclusive half-word.
  void ldxrh(const Register& rt, const MemOperand& src);

  // Load exclusive register.
  void ldxr(const Register& rt, const MemOperand& src);

  // Store exclusive register pair.
  void stxp(const Register& rs,
            const Register& rt,
            const Register& rt2,
            const MemOperand& dst);

  // Load exclusive register pair.
  void ldxp(const Register& rt, const Register& rt2, const MemOperand& src);

  // Store-release exclusive byte.
  void stlxrb(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store-release exclusive half-word.
  void stlxrh(const Register& rs, const Register& rt, const MemOperand& dst);

  // Store-release exclusive register.
  void stlxr(const Register& rs, const Register& rt, const MemOperand& dst);

  // Load-acquire exclusive byte.
  void ldaxrb(const Register& rt, const MemOperand& src);

  // Load-acquire exclusive half-word.
  void ldaxrh(const Register& rt, const MemOperand& src);

  // Load-acquire exclusive register.
  void ldaxr(const Register& rt, const MemOperand& src);

  // Store-release exclusive register pair.
  void stlxp(const Register& rs,
             const Register& rt,
             const Register& rt2,
             const MemOperand& dst);

  // Load-acquire exclusive register pair.
  void ldaxp(const Register& rt, const Register& rt2, const MemOperand& src);

  // Store-release byte.
  void stlrb(const Register& rt, const MemOperand& dst);

  // Store-release half-word.
  void stlrh(const Register& rt, const MemOperand& dst);

  // Store-release register.
  void stlr(const Register& rt, const MemOperand& dst);

  // Load-acquire byte.
  void ldarb(const Register& rt, const MemOperand& src);

  // Load-acquire half-word.
  void ldarh(const Register& rt, const MemOperand& src);

  // Load-acquire register.
  void ldar(const Register& rt, const MemOperand& src);

  // Store LORelease byte [Armv8.1].
  void stllrb(const Register& rt, const MemOperand& dst);

  // Store LORelease half-word [Armv8.1].
  void stllrh(const Register& rt, const MemOperand& dst);

  // Store LORelease register [Armv8.1].
  void stllr(const Register& rt, const MemOperand& dst);

  // Load LORelease byte [Armv8.1].
  void ldlarb(const Register& rt, const MemOperand& src);

  // Load LORelease half-word [Armv8.1].
  void ldlarh(const Register& rt, const MemOperand& src);

  // Load LORelease register [Armv8.1].
  void ldlar(const Register& rt, const MemOperand& src);

  // Compare and Swap word or doubleword in memory [Armv8.1].
  void cas(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap word or doubleword in memory [Armv8.1].
  void casa(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap word or doubleword in memory [Armv8.1].
  void casl(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap word or doubleword in memory [Armv8.1].
  void casal(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap byte in memory [Armv8.1].
  void casb(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap byte in memory [Armv8.1].
  void casab(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap byte in memory [Armv8.1].
  void caslb(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap byte in memory [Armv8.1].
  void casalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap halfword in memory [Armv8.1].
  void cash(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap halfword in memory [Armv8.1].
  void casah(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap halfword in memory [Armv8.1].
  void caslh(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap halfword in memory [Armv8.1].
  void casalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Compare and Swap Pair of words or doublewords in memory [Armv8.1].
  void casp(const Register& rs,
            const Register& rs2,
            const Register& rt,
            const Register& rt2,
            const MemOperand& src);

  // Compare and Swap Pair of words or doublewords in memory [Armv8.1].
  void caspa(const Register& rs,
             const Register& rs2,
             const Register& rt,
             const Register& rt2,
             const MemOperand& src);

  // Compare and Swap Pair of words or doublewords in memory [Armv8.1].
  void caspl(const Register& rs,
             const Register& rs2,
             const Register& rt,
             const Register& rt2,
             const MemOperand& src);

  // Compare and Swap Pair of words or doublewords in memory [Armv8.1].
  void caspal(const Register& rs,
              const Register& rs2,
              const Register& rt,
              const Register& rt2,
              const MemOperand& src);

  // Atomic add on byte in memory [Armv8.1]
  void ldaddb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on byte in memory, with Load-acquire semantics [Armv8.1]
  void ldaddab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on byte in memory, with Store-release semantics [Armv8.1]
  void ldaddlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on byte in memory, with Load-acquire and Store-release semantics
  // [Armv8.1]
  void ldaddalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on halfword in memory [Armv8.1]
  void ldaddh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on halfword in memory, with Load-acquire semantics [Armv8.1]
  void ldaddah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on halfword in memory, with Store-release semantics [Armv8.1]
  void ldaddlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on halfword in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void ldaddalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on word or doubleword in memory [Armv8.1]
  void ldadd(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on word or doubleword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldadda(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on word or doubleword in memory, with Store-release semantics
  // [Armv8.1]
  void ldaddl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on word or doubleword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldaddal(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on byte in memory [Armv8.1]
  void ldclrb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on byte in memory, with Load-acquire semantics [Armv8.1]
  void ldclrab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on byte in memory, with Store-release semantics [Armv8.1]
  void ldclrlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on byte in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void ldclralb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on halfword in memory [Armv8.1]
  void ldclrh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on halfword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldclrah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void ldclrlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on halfword in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void ldclralh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on word or doubleword in memory [Armv8.1]
  void ldclr(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on word or doubleword in memory, with Load-acquire
  // semantics [Armv8.1]
  void ldclra(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void ldclrl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit clear on word or doubleword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldclral(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on byte in memory [Armv8.1]
  void ldeorb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on byte in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldeorab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on byte in memory, with Store-release semantics
  // [Armv8.1]
  void ldeorlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on byte in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void ldeoralb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on halfword in memory [Armv8.1]
  void ldeorh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on halfword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldeorah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void ldeorlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on halfword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldeoralh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on word or doubleword in memory [Armv8.1]
  void ldeor(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on word or doubleword in memory, with Load-acquire
  // semantics [Armv8.1]
  void ldeora(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void ldeorl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic exclusive OR on word or doubleword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldeoral(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on byte in memory [Armv8.1]
  void ldsetb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on byte in memory, with Load-acquire semantics [Armv8.1]
  void ldsetab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on byte in memory, with Store-release semantics [Armv8.1]
  void ldsetlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on byte in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void ldsetalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on halfword in memory [Armv8.1]
  void ldseth(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on halfword in memory, with Load-acquire semantics [Armv8.1]
  void ldsetah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void ldsetlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on halfword in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void ldsetalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on word or doubleword in memory [Armv8.1]
  void ldset(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on word or doubleword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldseta(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void ldsetl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic bit set on word or doubleword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldsetal(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on byte in memory [Armv8.1]
  void ldsmaxb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on byte in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldsmaxab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on byte in memory, with Store-release semantics
  // [Armv8.1]
  void ldsmaxlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on byte in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldsmaxalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on halfword in memory [Armv8.1]
  void ldsmaxh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on halfword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldsmaxah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void ldsmaxlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on halfword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldsmaxalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on word or doubleword in memory [Armv8.1]
  void ldsmax(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on word or doubleword in memory, with Load-acquire
  // semantics [Armv8.1]
  void ldsmaxa(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void ldsmaxl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed maximum on word or doubleword in memory, with Load-acquire
  // and Store-release semantics [Armv8.1]
  void ldsmaxal(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on byte in memory [Armv8.1]
  void ldsminb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on byte in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldsminab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on byte in memory, with Store-release semantics
  // [Armv8.1]
  void ldsminlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on byte in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldsminalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on halfword in memory [Armv8.1]
  void ldsminh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on halfword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldsminah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void ldsminlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on halfword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldsminalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on word or doubleword in memory [Armv8.1]
  void ldsmin(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on word or doubleword in memory, with Load-acquire
  // semantics [Armv8.1]
  void ldsmina(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void ldsminl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic signed minimum on word or doubleword in memory, with Load-acquire
  // and Store-release semantics [Armv8.1]
  void ldsminal(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on byte in memory [Armv8.1]
  void ldumaxb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on byte in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldumaxab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on byte in memory, with Store-release semantics
  // [Armv8.1]
  void ldumaxlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on byte in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldumaxalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on halfword in memory [Armv8.1]
  void ldumaxh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on halfword in memory, with Load-acquire semantics
  // [Armv8.1]
  void ldumaxah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void ldumaxlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on halfword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void ldumaxalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on word or doubleword in memory [Armv8.1]
  void ldumax(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on word or doubleword in memory, with Load-acquire
  // semantics [Armv8.1]
  void ldumaxa(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void ldumaxl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned maximum on word or doubleword in memory, with Load-acquire
  // and Store-release semantics [Armv8.1]
  void ldumaxal(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on byte in memory [Armv8.1]
  void lduminb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on byte in memory, with Load-acquire semantics
  // [Armv8.1]
  void lduminab(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on byte in memory, with Store-release semantics
  // [Armv8.1]
  void lduminlb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on byte in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void lduminalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on halfword in memory [Armv8.1]
  void lduminh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on halfword in memory, with Load-acquire semantics
  // [Armv8.1]
  void lduminah(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on halfword in memory, with Store-release semantics
  // [Armv8.1]
  void lduminlh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on halfword in memory, with Load-acquire and
  // Store-release semantics [Armv8.1]
  void lduminalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on word or doubleword in memory [Armv8.1]
  void ldumin(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on word or doubleword in memory, with Load-acquire
  // semantics [Armv8.1]
  void ldumina(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on word or doubleword in memory, with Store-release
  // semantics [Armv8.1]
  void lduminl(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic unsigned minimum on word or doubleword in memory, with Load-acquire
  // and Store-release semantics [Armv8.1]
  void lduminal(const Register& rs, const Register& rt, const MemOperand& src);

  // Atomic add on byte in memory, without return. [Armv8.1]
  void staddb(const Register& rs, const MemOperand& src);

  // Atomic add on byte in memory, with Store-release semantics and without
  // return. [Armv8.1]
  void staddlb(const Register& rs, const MemOperand& src);

  // Atomic add on halfword in memory, without return. [Armv8.1]
  void staddh(const Register& rs, const MemOperand& src);

  // Atomic add on halfword in memory, with Store-release semantics and without
  // return. [Armv8.1]
  void staddlh(const Register& rs, const MemOperand& src);

  // Atomic add on word or doubleword in memory, without return. [Armv8.1]
  void stadd(const Register& rs, const MemOperand& src);

  // Atomic add on word or doubleword in memory, with Store-release semantics
  // and without return. [Armv8.1]
  void staddl(const Register& rs, const MemOperand& src);

  // Atomic bit clear on byte in memory, without return. [Armv8.1]
  void stclrb(const Register& rs, const MemOperand& src);

  // Atomic bit clear on byte in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stclrlb(const Register& rs, const MemOperand& src);

  // Atomic bit clear on halfword in memory, without return. [Armv8.1]
  void stclrh(const Register& rs, const MemOperand& src);

  // Atomic bit clear on halfword in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stclrlh(const Register& rs, const MemOperand& src);

  // Atomic bit clear on word or doubleword in memory, without return. [Armv8.1]
  void stclr(const Register& rs, const MemOperand& src);

  // Atomic bit clear on word or doubleword in memory, with Store-release
  // semantics and without return. [Armv8.1]
  void stclrl(const Register& rs, const MemOperand& src);

  // Atomic exclusive OR on byte in memory, without return. [Armv8.1]
  void steorb(const Register& rs, const MemOperand& src);

  // Atomic exclusive OR on byte in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void steorlb(const Register& rs, const MemOperand& src);

  // Atomic exclusive OR on halfword in memory, without return. [Armv8.1]
  void steorh(const Register& rs, const MemOperand& src);

  // Atomic exclusive OR on halfword in memory, with Store-release semantics
  // and without return. [Armv8.1]
  void steorlh(const Register& rs, const MemOperand& src);

  // Atomic exclusive OR on word or doubleword in memory, without return.
  // [Armv8.1]
  void steor(const Register& rs, const MemOperand& src);

  // Atomic exclusive OR on word or doubleword in memory, with Store-release
  // semantics and without return. [Armv8.1]
  void steorl(const Register& rs, const MemOperand& src);

  // Atomic bit set on byte in memory, without return. [Armv8.1]
  void stsetb(const Register& rs, const MemOperand& src);

  // Atomic bit set on byte in memory, with Store-release semantics and without
  // return. [Armv8.1]
  void stsetlb(const Register& rs, const MemOperand& src);

  // Atomic bit set on halfword in memory, without return. [Armv8.1]
  void stseth(const Register& rs, const MemOperand& src);

  // Atomic bit set on halfword in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stsetlh(const Register& rs, const MemOperand& src);

  // Atomic bit set on word or doubleword in memory, without return. [Armv8.1]
  void stset(const Register& rs, const MemOperand& src);

  // Atomic bit set on word or doubleword in memory, with Store-release
  // semantics and without return. [Armv8.1]
  void stsetl(const Register& rs, const MemOperand& src);

  // Atomic signed maximum on byte in memory, without return. [Armv8.1]
  void stsmaxb(const Register& rs, const MemOperand& src);

  // Atomic signed maximum on byte in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stsmaxlb(const Register& rs, const MemOperand& src);

  // Atomic signed maximum on halfword in memory, without return. [Armv8.1]
  void stsmaxh(const Register& rs, const MemOperand& src);

  // Atomic signed maximum on halfword in memory, with Store-release semantics
  // and without return. [Armv8.1]
  void stsmaxlh(const Register& rs, const MemOperand& src);

  // Atomic signed maximum on word or doubleword in memory, without return.
  // [Armv8.1]
  void stsmax(const Register& rs, const MemOperand& src);

  // Atomic signed maximum on word or doubleword in memory, with Store-release
  // semantics and without return. [Armv8.1]
  void stsmaxl(const Register& rs, const MemOperand& src);

  // Atomic signed minimum on byte in memory, without return. [Armv8.1]
  void stsminb(const Register& rs, const MemOperand& src);

  // Atomic signed minimum on byte in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stsminlb(const Register& rs, const MemOperand& src);

  // Atomic signed minimum on halfword in memory, without return. [Armv8.1]
  void stsminh(const Register& rs, const MemOperand& src);

  // Atomic signed minimum on halfword in memory, with Store-release semantics
  // and without return. [Armv8.1]
  void stsminlh(const Register& rs, const MemOperand& src);

  // Atomic signed minimum on word or doubleword in memory, without return.
  // [Armv8.1]
  void stsmin(const Register& rs, const MemOperand& src);

  // Atomic signed minimum on word or doubleword in memory, with Store-release
  // semantics and without return. semantics [Armv8.1]
  void stsminl(const Register& rs, const MemOperand& src);

  // Atomic unsigned maximum on byte in memory, without return. [Armv8.1]
  void stumaxb(const Register& rs, const MemOperand& src);

  // Atomic unsigned maximum on byte in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stumaxlb(const Register& rs, const MemOperand& src);

  // Atomic unsigned maximum on halfword in memory, without return. [Armv8.1]
  void stumaxh(const Register& rs, const MemOperand& src);

  // Atomic unsigned maximum on halfword in memory, with Store-release semantics
  // and without return. [Armv8.1]
  void stumaxlh(const Register& rs, const MemOperand& src);

  // Atomic unsigned maximum on word or doubleword in memory, without return.
  // [Armv8.1]
  void stumax(const Register& rs, const MemOperand& src);

  // Atomic unsigned maximum on word or doubleword in memory, with Store-release
  // semantics and without return. [Armv8.1]
  void stumaxl(const Register& rs, const MemOperand& src);

  // Atomic unsigned minimum on byte in memory, without return. [Armv8.1]
  void stuminb(const Register& rs, const MemOperand& src);

  // Atomic unsigned minimum on byte in memory, with Store-release semantics and
  // without return. [Armv8.1]
  void stuminlb(const Register& rs, const MemOperand& src);

  // Atomic unsigned minimum on halfword in memory, without return. [Armv8.1]
  void stuminh(const Register& rs, const MemOperand& src);

  // Atomic unsigned minimum on halfword in memory, with Store-release semantics
  // and without return. [Armv8.1]
  void stuminlh(const Register& rs, const MemOperand& src);

  // Atomic unsigned minimum on word or doubleword in memory, without return.
  // [Armv8.1]
  void stumin(const Register& rs, const MemOperand& src);

  // Atomic unsigned minimum on word or doubleword in memory, with Store-release
  // semantics and without return. [Armv8.1]
  void stuminl(const Register& rs, const MemOperand& src);

  // Swap byte in memory [Armv8.1]
  void swpb(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap byte in memory, with Load-acquire semantics [Armv8.1]
  void swpab(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap byte in memory, with Store-release semantics [Armv8.1]
  void swplb(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap byte in memory, with Load-acquire and Store-release semantics
  // [Armv8.1]
  void swpalb(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap halfword in memory [Armv8.1]
  void swph(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap halfword in memory, with Load-acquire semantics [Armv8.1]
  void swpah(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap halfword in memory, with Store-release semantics [Armv8.1]
  void swplh(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap halfword in memory, with Load-acquire and Store-release semantics
  // [Armv8.1]
  void swpalh(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap word or doubleword in memory [Armv8.1]
  void swp(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap word or doubleword in memory, with Load-acquire semantics [Armv8.1]
  void swpa(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap word or doubleword in memory, with Store-release semantics [Armv8.1]
  void swpl(const Register& rs, const Register& rt, const MemOperand& src);

  // Swap word or doubleword in memory, with Load-acquire and Store-release
  // semantics [Armv8.1]
  void swpal(const Register& rs, const Register& rt, const MemOperand& src);

  // Load-Acquire RCpc Register byte [Armv8.3]
  void ldaprb(const Register& rt, const MemOperand& src);

  // Load-Acquire RCpc Register halfword [Armv8.3]
  void ldaprh(const Register& rt, const MemOperand& src);

  // Load-Acquire RCpc Register word or doubleword [Armv8.3]
  void ldapr(const Register& rt, const MemOperand& src);

  // Prefetch memory.
  void prfm(PrefetchOperation op,
            const MemOperand& addr,
            LoadStoreScalingOption option = PreferScaledOffset);

  // Prefetch memory (with unscaled offset).
  void prfum(PrefetchOperation op,
             const MemOperand& addr,
             LoadStoreScalingOption option = PreferUnscaledOffset);

  // Prefetch memory in the literal pool.
  void prfm(PrefetchOperation op, RawLiteral* literal);

  // Prefetch from pc + imm19 << 2.
  void prfm(PrefetchOperation op, int64_t imm19);

  // Move instructions. The default shift of -1 indicates that the move
  // instruction will calculate an appropriate 16-bit immediate and left shift
  // that is equal to the 64-bit immediate argument. If an explicit left shift
  // is specified (0, 16, 32 or 48), the immediate must be a 16-bit value.
  //
  // For movk, an explicit shift can be used to indicate which half word should
  // be overwritten, eg. movk(x0, 0, 0) will overwrite the least-significant
  // half word with zero, whereas movk(x0, 0, 48) will overwrite the
  // most-significant.

  // Move immediate and keep.
  void movk(const Register& rd, uint64_t imm, int shift = -1) {
    MoveWide(rd, imm, shift, MOVK);
  }

  // Move inverted immediate.
  void movn(const Register& rd, uint64_t imm, int shift = -1) {
    MoveWide(rd, imm, shift, MOVN);
  }

  // Move immediate.
  void movz(const Register& rd, uint64_t imm, int shift = -1) {
    MoveWide(rd, imm, shift, MOVZ);
  }

  // Misc instructions.
  // Monitor debug-mode breakpoint.
  void brk(int code);

  // Halting debug-mode breakpoint.
  void hlt(int code);

  // Generate exception targeting EL1.
  void svc(int code);

  // Move register to register.
  void mov(const Register& rd, const Register& rn);

  // Move inverted operand to register.
  void mvn(const Register& rd, const Operand& operand);

  // System instructions.
  // Move to register from system register.
  void mrs(const Register& xt, SystemRegister sysreg);

  // Move from register to system register.
  void msr(SystemRegister sysreg, const Register& xt);

  // System instruction.
  void sys(int op1, int crn, int crm, int op2, const Register& xt = xzr);

  // System instruction with pre-encoded op (op1:crn:crm:op2).
  void sys(int op, const Register& xt = xzr);

  // System data cache operation.
  void dc(DataCacheOp op, const Register& rt);

  // System instruction cache operation.
  void ic(InstructionCacheOp op, const Register& rt);

  // System hint (named type).
  void hint(SystemHint code);

  // System hint (numbered type).
  void hint(int imm7);

  // Clear exclusive monitor.
  void clrex(int imm4 = 0xf);

  // Data memory barrier.
  void dmb(BarrierDomain domain, BarrierType type);

  // Data synchronization barrier.
  void dsb(BarrierDomain domain, BarrierType type);

  // Instruction synchronization barrier.
  void isb();

  // Error synchronization barrier.
  void esb();

  // Conditional speculation dependency barrier.
  void csdb();

  // Alias for system instructions.
  // No-op.
  void nop() { hint(NOP); }

  // FP and NEON instructions.
  // Move double precision immediate to FP register.
  void fmov(const VRegister& vd, double imm);

  // Move single precision immediate to FP register.
  void fmov(const VRegister& vd, float imm);

  // Move half precision immediate to FP register [Armv8.2].
  void fmov(const VRegister& vd, Float16 imm);

  // Move FP register to register.
  void fmov(const Register& rd, const VRegister& fn);

  // Move register to FP register.
  void fmov(const VRegister& vd, const Register& rn);

  // Move FP register to FP register.
  void fmov(const VRegister& vd, const VRegister& fn);

  // Move 64-bit register to top half of 128-bit FP register.
  void fmov(const VRegister& vd, int index, const Register& rn);

  // Move top half of 128-bit FP register to 64-bit register.
  void fmov(const Register& rd, const VRegister& vn, int index);

  // FP add.
  void fadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP subtract.
  void fsub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP multiply.
  void fmul(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP fused multiply-add.
  void fmadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va);

  // FP fused multiply-subtract.
  void fmsub(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             const VRegister& va);

  // FP fused multiply-add and negate.
  void fnmadd(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va);

  // FP fused multiply-subtract and negate.
  void fnmsub(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              const VRegister& va);

  // FP multiply-negate scalar.
  void fnmul(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP reciprocal exponent scalar.
  void frecpx(const VRegister& vd, const VRegister& vn);

  // FP divide.
  void fdiv(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP maximum.
  void fmax(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP minimum.
  void fmin(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP maximum number.
  void fmaxnm(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP minimum number.
  void fminnm(const VRegister& vd, const VRegister& fn, const VRegister& vm);

  // FP absolute.
  void fabs(const VRegister& vd, const VRegister& vn);

  // FP negate.
  void fneg(const VRegister& vd, const VRegister& vn);

  // FP square root.
  void fsqrt(const VRegister& vd, const VRegister& vn);

  // FP round to integer, nearest with ties to away.
  void frinta(const VRegister& vd, const VRegister& vn);

  // FP round to integer, implicit rounding.
  void frinti(const VRegister& vd, const VRegister& vn);

  // FP round to integer, toward minus infinity.
  void frintm(const VRegister& vd, const VRegister& vn);

  // FP round to integer, nearest with ties to even.
  void frintn(const VRegister& vd, const VRegister& vn);

  // FP round to integer, toward plus infinity.
  void frintp(const VRegister& vd, const VRegister& vn);

  // FP round to integer, exact, implicit rounding.
  void frintx(const VRegister& vd, const VRegister& vn);

  // FP round to integer, towards zero.
  void frintz(const VRegister& vd, const VRegister& vn);

  void FPCompareMacro(const VRegister& vn, double value, FPTrapFlags trap);

  void FPCompareMacro(const VRegister& vn,
                      const VRegister& vm,
                      FPTrapFlags trap);

  // FP compare registers.
  void fcmp(const VRegister& vn, const VRegister& vm);

  // FP compare immediate.
  void fcmp(const VRegister& vn, double value);

  void FPCCompareMacro(const VRegister& vn,
                       const VRegister& vm,
                       StatusFlags nzcv,
                       Condition cond,
                       FPTrapFlags trap);

  // FP conditional compare.
  void fccmp(const VRegister& vn,
             const VRegister& vm,
             StatusFlags nzcv,
             Condition cond);

  // FP signaling compare registers.
  void fcmpe(const VRegister& vn, const VRegister& vm);

  // FP signaling compare immediate.
  void fcmpe(const VRegister& vn, double value);

  // FP conditional signaling compare.
  void fccmpe(const VRegister& vn,
              const VRegister& vm,
              StatusFlags nzcv,
              Condition cond);

  // FP conditional select.
  void fcsel(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             Condition cond);

  // Common FP Convert functions.
  void NEONFPConvertToInt(const Register& rd, const VRegister& vn, Instr op);
  void NEONFPConvertToInt(const VRegister& vd, const VRegister& vn, Instr op);
  void NEONFP16ConvertToInt(const VRegister& vd, const VRegister& vn, Instr op);

  // FP convert between precisions.
  void fcvt(const VRegister& vd, const VRegister& vn);

  // FP convert to higher precision.
  void fcvtl(const VRegister& vd, const VRegister& vn);

  // FP convert to higher precision (second part).
  void fcvtl2(const VRegister& vd, const VRegister& vn);

  // FP convert to lower precision.
  void fcvtn(const VRegister& vd, const VRegister& vn);

  // FP convert to lower prevision (second part).
  void fcvtn2(const VRegister& vd, const VRegister& vn);

  // FP convert to lower precision, rounding to odd.
  void fcvtxn(const VRegister& vd, const VRegister& vn);

  // FP convert to lower precision, rounding to odd (second part).
  void fcvtxn2(const VRegister& vd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to away.
  void fcvtas(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to away.
  void fcvtau(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to away.
  void fcvtas(const VRegister& vd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to away.
  void fcvtau(const VRegister& vd, const VRegister& vn);

  // FP convert to signed integer, round towards -infinity.
  void fcvtms(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, round towards -infinity.
  void fcvtmu(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, round towards -infinity.
  void fcvtms(const VRegister& vd, const VRegister& vn);

  // FP convert to unsigned integer, round towards -infinity.
  void fcvtmu(const VRegister& vd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to even.
  void fcvtns(const Register& rd, const VRegister& vn);

  // FP JavaScript convert to signed integer, rounding toward zero [Armv8.3].
  void fjcvtzs(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to even.
  void fcvtnu(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, nearest with ties to even.
  void fcvtns(const VRegister& rd, const VRegister& vn);

  // FP convert to unsigned integer, nearest with ties to even.
  void fcvtnu(const VRegister& rd, const VRegister& vn);

  // FP convert to signed integer or fixed-point, round towards zero.
  void fcvtzs(const Register& rd, const VRegister& vn, int fbits = 0);

  // FP convert to unsigned integer or fixed-point, round towards zero.
  void fcvtzu(const Register& rd, const VRegister& vn, int fbits = 0);

  // FP convert to signed integer or fixed-point, round towards zero.
  void fcvtzs(const VRegister& vd, const VRegister& vn, int fbits = 0);

  // FP convert to unsigned integer or fixed-point, round towards zero.
  void fcvtzu(const VRegister& vd, const VRegister& vn, int fbits = 0);

  // FP convert to signed integer, round towards +infinity.
  void fcvtps(const Register& rd, const VRegister& vn);

  // FP convert to unsigned integer, round towards +infinity.
  void fcvtpu(const Register& rd, const VRegister& vn);

  // FP convert to signed integer, round towards +infinity.
  void fcvtps(const VRegister& vd, const VRegister& vn);

  // FP convert to unsigned integer, round towards +infinity.
  void fcvtpu(const VRegister& vd, const VRegister& vn);

  // Convert signed integer or fixed point to FP.
  void scvtf(const VRegister& fd, const Register& rn, int fbits = 0);

  // Convert unsigned integer or fixed point to FP.
  void ucvtf(const VRegister& fd, const Register& rn, int fbits = 0);

  // Convert signed integer or fixed-point to FP.
  void scvtf(const VRegister& fd, const VRegister& vn, int fbits = 0);

  // Convert unsigned integer or fixed-point to FP.
  void ucvtf(const VRegister& fd, const VRegister& vn, int fbits = 0);

  // Unsigned absolute difference.
  void uabd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed absolute difference.
  void sabd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned absolute difference and accumulate.
  void uaba(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed absolute difference and accumulate.
  void saba(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Add.
  void add(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Subtract.
  void sub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned halving add.
  void uhadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed halving add.
  void shadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned rounding halving add.
  void urhadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed rounding halving add.
  void srhadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned halving sub.
  void uhsub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed halving sub.
  void shsub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned saturating add.
  void uqadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating add.
  void sqadd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned saturating subtract.
  void uqsub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating subtract.
  void sqsub(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Add pairwise.
  void addp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Add pair of elements scalar.
  void addp(const VRegister& vd, const VRegister& vn);

  // Multiply-add to accumulator.
  void mla(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Multiply-subtract to accumulator.
  void mls(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Multiply.
  void mul(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Multiply by scalar element.
  void mul(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int vm_index);

  // Multiply-add by scalar element.
  void mla(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int vm_index);

  // Multiply-subtract by scalar element.
  void mls(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int vm_index);

  // Signed long multiply-add by scalar element.
  void smlal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Signed long multiply-add by scalar element (second part).
  void smlal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Unsigned long multiply-add by scalar element.
  void umlal(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Unsigned long multiply-add by scalar element (second part).
  void umlal2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Signed long multiply-sub by scalar element.
  void smlsl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Signed long multiply-sub by scalar element (second part).
  void smlsl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Unsigned long multiply-sub by scalar element.
  void umlsl(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Unsigned long multiply-sub by scalar element (second part).
  void umlsl2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Signed long multiply by scalar element.
  void smull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Signed long multiply by scalar element (second part).
  void smull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Unsigned long multiply by scalar element.
  void umull(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // Unsigned long multiply by scalar element (second part).
  void umull2(const VRegister& vd,
              const VRegister& vn,
              const VRegister& vm,
              int vm_index);

  // Signed saturating double long multiply by element.
  void sqdmull(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating double long multiply by element (second part).
  void sqdmull2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Signed saturating doubling long multiply-add by element.
  void sqdmlal(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating doubling long multiply-add by element (second part).
  void sqdmlal2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Signed saturating doubling long multiply-sub by element.
  void sqdmlsl(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating doubling long multiply-sub by element (second part).
  void sqdmlsl2(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Compare equal.
  void cmeq(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Compare signed greater than or equal.
  void cmge(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Compare signed greater than.
  void cmgt(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Compare unsigned higher.
  void cmhi(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Compare unsigned higher or same.
  void cmhs(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Compare bitwise test bits nonzero.
  void cmtst(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Compare bitwise to zero.
  void cmeq(const VRegister& vd, const VRegister& vn, int value);

  // Compare signed greater than or equal to zero.
  void cmge(const VRegister& vd, const VRegister& vn, int value);

  // Compare signed greater than zero.
  void cmgt(const VRegister& vd, const VRegister& vn, int value);

  // Compare signed less than or equal to zero.
  void cmle(const VRegister& vd, const VRegister& vn, int value);

  // Compare signed less than zero.
  void cmlt(const VRegister& vd, const VRegister& vn, int value);

  // Signed shift left by register.
  void sshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned shift left by register.
  void ushl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating shift left by register.
  void sqshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned saturating shift left by register.
  void uqshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed rounding shift left by register.
  void srshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned rounding shift left by register.
  void urshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating rounding shift left by register.
  void sqrshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned saturating rounding shift left by register.
  void uqrshl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise and.
  void and_(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise or.
  void orr(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise or immediate.
  void orr(const VRegister& vd, const int imm8, const int left_shift = 0);

  // Move register to register.
  void mov(const VRegister& vd, const VRegister& vn);

  // Bitwise orn.
  void orn(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise eor.
  void eor(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bit clear immediate.
  void bic(const VRegister& vd, const int imm8, const int left_shift = 0);

  // Bit clear.
  void bic(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise insert if false.
  void bif(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise insert if true.
  void bit(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Bitwise select.
  void bsl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Polynomial multiply.
  void pmul(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Vector move immediate.
  void movi(const VRegister& vd,
            const uint64_t imm,
            Shift shift = LSL,
            const int shift_amount = 0);

  // Bitwise not.
  void mvn(const VRegister& vd, const VRegister& vn);

  // Vector move inverted immediate.
  void mvni(const VRegister& vd,
            const int imm8,
            Shift shift = LSL,
            const int shift_amount = 0);

  // Signed saturating accumulate of unsigned value.
  void suqadd(const VRegister& vd, const VRegister& vn);

  // Unsigned saturating accumulate of signed value.
  void usqadd(const VRegister& vd, const VRegister& vn);

  // Absolute value.
  void abs(const VRegister& vd, const VRegister& vn);

  // Signed saturating absolute value.
  void sqabs(const VRegister& vd, const VRegister& vn);

  // Negate.
  void neg(const VRegister& vd, const VRegister& vn);

  // Signed saturating negate.
  void sqneg(const VRegister& vd, const VRegister& vn);

  // Bitwise not.
  void not_(const VRegister& vd, const VRegister& vn);

  // Extract narrow.
  void xtn(const VRegister& vd, const VRegister& vn);

  // Extract narrow (second part).
  void xtn2(const VRegister& vd, const VRegister& vn);

  // Signed saturating extract narrow.
  void sqxtn(const VRegister& vd, const VRegister& vn);

  // Signed saturating extract narrow (second part).
  void sqxtn2(const VRegister& vd, const VRegister& vn);

  // Unsigned saturating extract narrow.
  void uqxtn(const VRegister& vd, const VRegister& vn);

  // Unsigned saturating extract narrow (second part).
  void uqxtn2(const VRegister& vd, const VRegister& vn);

  // Signed saturating extract unsigned narrow.
  void sqxtun(const VRegister& vd, const VRegister& vn);

  // Signed saturating extract unsigned narrow (second part).
  void sqxtun2(const VRegister& vd, const VRegister& vn);

  // Extract vector from pair of vectors.
  void ext(const VRegister& vd,
           const VRegister& vn,
           const VRegister& vm,
           int index);

  // Duplicate vector element to vector or scalar.
  void dup(const VRegister& vd, const VRegister& vn, int vn_index);

  // Move vector element to scalar.
  void mov(const VRegister& vd, const VRegister& vn, int vn_index);

  // Duplicate general-purpose register to vector.
  void dup(const VRegister& vd, const Register& rn);

  // Insert vector element from another vector element.
  void ins(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index);

  // Move vector element to another vector element.
  void mov(const VRegister& vd,
           int vd_index,
           const VRegister& vn,
           int vn_index);

  // Insert vector element from general-purpose register.
  void ins(const VRegister& vd, int vd_index, const Register& rn);

  // Move general-purpose register to a vector element.
  void mov(const VRegister& vd, int vd_index, const Register& rn);

  // Unsigned move vector element to general-purpose register.
  void umov(const Register& rd, const VRegister& vn, int vn_index);

  // Move vector element to general-purpose register.
  void mov(const Register& rd, const VRegister& vn, int vn_index);

  // Signed move vector element to general-purpose register.
  void smov(const Register& rd, const VRegister& vn, int vn_index);

  // One-element structure load to one register.
  void ld1(const VRegister& vt, const MemOperand& src);

  // One-element structure load to two registers.
  void ld1(const VRegister& vt, const VRegister& vt2, const MemOperand& src);

  // One-element structure load to three registers.
  void ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // One-element structure load to four registers.
  void ld1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // One-element single structure load to one lane.
  void ld1(const VRegister& vt, int lane, const MemOperand& src);

  // One-element single structure load to all lanes.
  void ld1r(const VRegister& vt, const MemOperand& src);

  // Two-element structure load.
  void ld2(const VRegister& vt, const VRegister& vt2, const MemOperand& src);

  // Two-element single structure load to one lane.
  void ld2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& src);

  // Two-element single structure load to all lanes.
  void ld2r(const VRegister& vt, const VRegister& vt2, const MemOperand& src);

  // Three-element structure load.
  void ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // Three-element single structure load to one lane.
  void ld3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& src);

  // Three-element single structure load to all lanes.
  void ld3r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const MemOperand& src);

  // Four-element structure load.
  void ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // Four-element single structure load to one lane.
  void ld4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& src);

  // Four-element single structure load to all lanes.
  void ld4r(const VRegister& vt,
            const VRegister& vt2,
            const VRegister& vt3,
            const VRegister& vt4,
            const MemOperand& src);

  // Count leading sign bits.
  void cls(const VRegister& vd, const VRegister& vn);

  // Count leading zero bits (vector).
  void clz(const VRegister& vd, const VRegister& vn);

  // Population count per byte.
  void cnt(const VRegister& vd, const VRegister& vn);

  // Reverse bit order.
  void rbit(const VRegister& vd, const VRegister& vn);

  // Reverse elements in 16-bit halfwords.
  void rev16(const VRegister& vd, const VRegister& vn);

  // Reverse elements in 32-bit words.
  void rev32(const VRegister& vd, const VRegister& vn);

  // Reverse elements in 64-bit doublewords.
  void rev64(const VRegister& vd, const VRegister& vn);

  // Unsigned reciprocal square root estimate.
  void ursqrte(const VRegister& vd, const VRegister& vn);

  // Unsigned reciprocal estimate.
  void urecpe(const VRegister& vd, const VRegister& vn);

  // Signed pairwise long add.
  void saddlp(const VRegister& vd, const VRegister& vn);

  // Unsigned pairwise long add.
  void uaddlp(const VRegister& vd, const VRegister& vn);

  // Signed pairwise long add and accumulate.
  void sadalp(const VRegister& vd, const VRegister& vn);

  // Unsigned pairwise long add and accumulate.
  void uadalp(const VRegister& vd, const VRegister& vn);

  // Shift left by immediate.
  void shl(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating shift left by immediate.
  void sqshl(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating shift left unsigned by immediate.
  void sqshlu(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned saturating shift left by immediate.
  void uqshl(const VRegister& vd, const VRegister& vn, int shift);

  // Signed shift left long by immediate.
  void sshll(const VRegister& vd, const VRegister& vn, int shift);

  // Signed shift left long by immediate (second part).
  void sshll2(const VRegister& vd, const VRegister& vn, int shift);

  // Signed extend long.
  void sxtl(const VRegister& vd, const VRegister& vn);

  // Signed extend long (second part).
  void sxtl2(const VRegister& vd, const VRegister& vn);

  // Unsigned shift left long by immediate.
  void ushll(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned shift left long by immediate (second part).
  void ushll2(const VRegister& vd, const VRegister& vn, int shift);

  // Shift left long by element size.
  void shll(const VRegister& vd, const VRegister& vn, int shift);

  // Shift left long by element size (second part).
  void shll2(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned extend long.
  void uxtl(const VRegister& vd, const VRegister& vn);

  // Unsigned extend long (second part).
  void uxtl2(const VRegister& vd, const VRegister& vn);

  // Shift left by immediate and insert.
  void sli(const VRegister& vd, const VRegister& vn, int shift);

  // Shift right by immediate and insert.
  void sri(const VRegister& vd, const VRegister& vn, int shift);

  // Signed maximum.
  void smax(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed pairwise maximum.
  void smaxp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Add across vector.
  void addv(const VRegister& vd, const VRegister& vn);

  // Signed add long across vector.
  void saddlv(const VRegister& vd, const VRegister& vn);

  // Unsigned add long across vector.
  void uaddlv(const VRegister& vd, const VRegister& vn);

  // FP maximum number across vector.
  void fmaxnmv(const VRegister& vd, const VRegister& vn);

  // FP maximum across vector.
  void fmaxv(const VRegister& vd, const VRegister& vn);

  // FP minimum number across vector.
  void fminnmv(const VRegister& vd, const VRegister& vn);

  // FP minimum across vector.
  void fminv(const VRegister& vd, const VRegister& vn);

  // Signed maximum across vector.
  void smaxv(const VRegister& vd, const VRegister& vn);

  // Signed minimum.
  void smin(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed minimum pairwise.
  void sminp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed minimum across vector.
  void sminv(const VRegister& vd, const VRegister& vn);

  // One-element structure store from one register.
  void st1(const VRegister& vt, const MemOperand& src);

  // One-element structure store from two registers.
  void st1(const VRegister& vt, const VRegister& vt2, const MemOperand& src);

  // One-element structure store from three registers.
  void st1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // One-element structure store from four registers.
  void st1(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // One-element single structure store from one lane.
  void st1(const VRegister& vt, int lane, const MemOperand& src);

  // Two-element structure store from two registers.
  void st2(const VRegister& vt, const VRegister& vt2, const MemOperand& src);

  // Two-element single structure store from two lanes.
  void st2(const VRegister& vt,
           const VRegister& vt2,
           int lane,
           const MemOperand& src);

  // Three-element structure store from three registers.
  void st3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const MemOperand& src);

  // Three-element single structure store from three lanes.
  void st3(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           int lane,
           const MemOperand& src);

  // Four-element structure store from four registers.
  void st4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           const MemOperand& src);

  // Four-element single structure store from four lanes.
  void st4(const VRegister& vt,
           const VRegister& vt2,
           const VRegister& vt3,
           const VRegister& vt4,
           int lane,
           const MemOperand& src);

  // Unsigned add long.
  void uaddl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned add long (second part).
  void uaddl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned add wide.
  void uaddw(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned add wide (second part).
  void uaddw2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed add long.
  void saddl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed add long (second part).
  void saddl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed add wide.
  void saddw(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed add wide (second part).
  void saddw2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned subtract long.
  void usubl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned subtract long (second part).
  void usubl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned subtract wide.
  void usubw(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned subtract wide (second part).
  void usubw2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed subtract long.
  void ssubl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed subtract long (second part).
  void ssubl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed integer subtract wide.
  void ssubw(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed integer subtract wide (second part).
  void ssubw2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned maximum.
  void umax(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned pairwise maximum.
  void umaxp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned maximum across vector.
  void umaxv(const VRegister& vd, const VRegister& vn);

  // Unsigned minimum.
  void umin(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned pairwise minimum.
  void uminp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned minimum across vector.
  void uminv(const VRegister& vd, const VRegister& vn);

  // Transpose vectors (primary).
  void trn1(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Transpose vectors (secondary).
  void trn2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unzip vectors (primary).
  void uzp1(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unzip vectors (secondary).
  void uzp2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Zip vectors (primary).
  void zip1(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Zip vectors (secondary).
  void zip2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed shift right by immediate.
  void sshr(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned shift right by immediate.
  void ushr(const VRegister& vd, const VRegister& vn, int shift);

  // Signed rounding shift right by immediate.
  void srshr(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned rounding shift right by immediate.
  void urshr(const VRegister& vd, const VRegister& vn, int shift);

  // Signed shift right by immediate and accumulate.
  void ssra(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned shift right by immediate and accumulate.
  void usra(const VRegister& vd, const VRegister& vn, int shift);

  // Signed rounding shift right by immediate and accumulate.
  void srsra(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned rounding shift right by immediate and accumulate.
  void ursra(const VRegister& vd, const VRegister& vn, int shift);

  // Shift right narrow by immediate.
  void shrn(const VRegister& vd, const VRegister& vn, int shift);

  // Shift right narrow by immediate (second part).
  void shrn2(const VRegister& vd, const VRegister& vn, int shift);

  // Rounding shift right narrow by immediate.
  void rshrn(const VRegister& vd, const VRegister& vn, int shift);

  // Rounding shift right narrow by immediate (second part).
  void rshrn2(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned saturating shift right narrow by immediate.
  void uqshrn(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned saturating shift right narrow by immediate (second part).
  void uqshrn2(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned saturating rounding shift right narrow by immediate.
  void uqrshrn(const VRegister& vd, const VRegister& vn, int shift);

  // Unsigned saturating rounding shift right narrow by immediate (second part).
  void uqrshrn2(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating shift right narrow by immediate.
  void sqshrn(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating shift right narrow by immediate (second part).
  void sqshrn2(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating rounded shift right narrow by immediate.
  void sqrshrn(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating rounded shift right narrow by immediate (second part).
  void sqrshrn2(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating shift right unsigned narrow by immediate.
  void sqshrun(const VRegister& vd, const VRegister& vn, int shift);

  // Signed saturating shift right unsigned narrow by immediate (second part).
  void sqshrun2(const VRegister& vd, const VRegister& vn, int shift);

  // Signed sat rounded shift right unsigned narrow by immediate.
  void sqrshrun(const VRegister& vd, const VRegister& vn, int shift);

  // Signed sat rounded shift right unsigned narrow by immediate (second part).
  void sqrshrun2(const VRegister& vd, const VRegister& vn, int shift);

  // FP reciprocal step.
  void frecps(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP reciprocal estimate.
  void frecpe(const VRegister& vd, const VRegister& vn);

  // FP reciprocal square root estimate.
  void frsqrte(const VRegister& vd, const VRegister& vn);

  // FP reciprocal square root step.
  void frsqrts(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed absolute difference and accumulate long.
  void sabal(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed absolute difference and accumulate long (second part).
  void sabal2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned absolute difference and accumulate long.
  void uabal(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned absolute difference and accumulate long (second part).
  void uabal2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed absolute difference long.
  void sabdl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed absolute difference long (second part).
  void sabdl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned absolute difference long.
  void uabdl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned absolute difference long (second part).
  void uabdl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Polynomial multiply long.
  void pmull(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Polynomial multiply long (second part).
  void pmull2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed long multiply-add.
  void smlal(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed long multiply-add (second part).
  void smlal2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned long multiply-add.
  void umlal(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned long multiply-add (second part).
  void umlal2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed long multiply-sub.
  void smlsl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed long multiply-sub (second part).
  void smlsl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned long multiply-sub.
  void umlsl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned long multiply-sub (second part).
  void umlsl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed long multiply.
  void smull(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed long multiply (second part).
  void smull2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling long multiply-add.
  void sqdmlal(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling long multiply-add (second part).
  void sqdmlal2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling long multiply-subtract.
  void sqdmlsl(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling long multiply-subtract (second part).
  void sqdmlsl2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling long multiply.
  void sqdmull(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling long multiply (second part).
  void sqdmull2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling multiply returning high half.
  void sqdmulh(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating rounding doubling multiply returning high half.
  void sqrdmulh(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed dot product [Armv8.2].
  void sdot(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating rounding doubling multiply accumulate returning high
  // half [Armv8.1].
  void sqrdmlah(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned dot product [Armv8.2].
  void udot(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating rounding doubling multiply subtract returning high half
  // [Armv8.1].
  void sqrdmlsh(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Signed saturating doubling multiply element returning high half.
  void sqdmulh(const VRegister& vd,
               const VRegister& vn,
               const VRegister& vm,
               int vm_index);

  // Signed saturating rounding doubling multiply element returning high half.
  void sqrdmulh(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Signed dot product by element [Armv8.2].
  void sdot(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // Signed saturating rounding doubling multiply accumulate element returning
  // high half [Armv8.1].
  void sqrdmlah(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Unsigned dot product by element [Armv8.2].
  void udot(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // Signed saturating rounding doubling multiply subtract element returning
  // high half [Armv8.1].
  void sqrdmlsh(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                int vm_index);

  // Unsigned long multiply long.
  void umull(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Unsigned long multiply (second part).
  void umull2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Add narrow returning high half.
  void addhn(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Add narrow returning high half (second part).
  void addhn2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Rounding add narrow returning high half.
  void raddhn(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Rounding add narrow returning high half (second part).
  void raddhn2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Subtract narrow returning high half.
  void subhn(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Subtract narrow returning high half (second part).
  void subhn2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Rounding subtract narrow returning high half.
  void rsubhn(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // Rounding subtract narrow returning high half (second part).
  void rsubhn2(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP vector multiply accumulate.
  void fmla(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP vector multiply subtract.
  void fmls(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP vector multiply extended.
  void fmulx(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP absolute greater than or equal.
  void facge(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP absolute greater than.
  void facgt(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP multiply by element.
  void fmul(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // FP fused multiply-add to accumulator by element.
  void fmla(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // FP fused multiply-sub from accumulator by element.
  void fmls(const VRegister& vd,
            const VRegister& vn,
            const VRegister& vm,
            int vm_index);

  // FP multiply extended by element.
  void fmulx(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index);

  // FP compare equal.
  void fcmeq(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP greater than.
  void fcmgt(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP greater than or equal.
  void fcmge(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP compare equal to zero.
  void fcmeq(const VRegister& vd, const VRegister& vn, double imm);

  // FP greater than zero.
  void fcmgt(const VRegister& vd, const VRegister& vn, double imm);

  // FP greater than or equal to zero.
  void fcmge(const VRegister& vd, const VRegister& vn, double imm);

  // FP less than or equal to zero.
  void fcmle(const VRegister& vd, const VRegister& vn, double imm);

  // FP less than to zero.
  void fcmlt(const VRegister& vd, const VRegister& vn, double imm);

  // FP absolute difference.
  void fabd(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP pairwise add vector.
  void faddp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP pairwise add scalar.
  void faddp(const VRegister& vd, const VRegister& vn);

  // FP pairwise maximum vector.
  void fmaxp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP pairwise maximum scalar.
  void fmaxp(const VRegister& vd, const VRegister& vn);

  // FP pairwise minimum vector.
  void fminp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP pairwise minimum scalar.
  void fminp(const VRegister& vd, const VRegister& vn);

  // FP pairwise maximum number vector.
  void fmaxnmp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP pairwise maximum number scalar.
  void fmaxnmp(const VRegister& vd, const VRegister& vn);

  // FP pairwise minimum number vector.
  void fminnmp(const VRegister& vd, const VRegister& vn, const VRegister& vm);

  // FP pairwise minimum number scalar.
  void fminnmp(const VRegister& vd, const VRegister& vn);

  // v8.3 complex numbers - note that these are only partial/helper functions
  // and must be used in series in order to perform full CN operations.
  // FP complex multiply accumulate (by element) [Armv8.3].
  void fcmla(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int vm_index,
             int rot);

  // FP complex multiply accumulate [Armv8.3].
  void fcmla(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int rot);

  // FP complex add [Armv8.3].
  void fcadd(const VRegister& vd,
             const VRegister& vn,
             const VRegister& vm,
             int rot);

  // Emit generic instructions.
  // Emit raw instructions into the instruction stream.
  void dci(Instr raw_inst) { Emit(raw_inst); }

  // Emit 32 bits of data into the instruction stream.
  void dc32(uint32_t data) { dc(data); }

  // Emit 64 bits of data into the instruction stream.
  void dc64(uint64_t data) { dc(data); }

  // Emit data in the instruction stream.
  template <typename T>
  void dc(T data) {
    VIXL_ASSERT(AllowAssembler());
    GetBuffer()->Emit<T>(data);
  }

  // Copy a string into the instruction stream, including the terminating NULL
  // character. The instruction pointer is then aligned correctly for
  // subsequent instructions.
  void EmitString(const char* string) {
    VIXL_ASSERT(string != NULL);
    VIXL_ASSERT(AllowAssembler());

    GetBuffer()->EmitString(string);
    GetBuffer()->Align();
  }

  // Code generation helpers.

  // Register encoding.
  static Instr Rd(CPURegister rd) {
    VIXL_ASSERT(rd.GetCode() != kSPRegInternalCode);
    return rd.GetCode() << Rd_offset;
  }

  static Instr Rn(CPURegister rn) {
    VIXL_ASSERT(rn.GetCode() != kSPRegInternalCode);
    return rn.GetCode() << Rn_offset;
  }

  static Instr Rm(CPURegister rm) {
    VIXL_ASSERT(rm.GetCode() != kSPRegInternalCode);
    return rm.GetCode() << Rm_offset;
  }

  static Instr RmNot31(CPURegister rm) {
    VIXL_ASSERT(rm.GetCode() != kSPRegInternalCode);
    VIXL_ASSERT(!rm.IsZero());
    return Rm(rm);
  }

  static Instr Ra(CPURegister ra) {
    VIXL_ASSERT(ra.GetCode() != kSPRegInternalCode);
    return ra.GetCode() << Ra_offset;
  }

  static Instr Rt(CPURegister rt) {
    VIXL_ASSERT(rt.GetCode() != kSPRegInternalCode);
    return rt.GetCode() << Rt_offset;
  }

  static Instr Rt2(CPURegister rt2) {
    VIXL_ASSERT(rt2.GetCode() != kSPRegInternalCode);
    return rt2.GetCode() << Rt2_offset;
  }

  static Instr Rs(CPURegister rs) {
    VIXL_ASSERT(rs.GetCode() != kSPRegInternalCode);
    return rs.GetCode() << Rs_offset;
  }

  // These encoding functions allow the stack pointer to be encoded, and
  // disallow the zero register.
  static Instr RdSP(Register rd) {
    VIXL_ASSERT(!rd.IsZero());
    return (rd.GetCode() & kRegCodeMask) << Rd_offset;
  }

  static Instr RnSP(Register rn) {
    VIXL_ASSERT(!rn.IsZero());
    return (rn.GetCode() & kRegCodeMask) << Rn_offset;
  }

  static Instr RmSP(Register rm) {
    VIXL_ASSERT(!rm.IsZero());
    return (rm.GetCode() & kRegCodeMask) << Rm_offset;
  }

  // Flags encoding.
  static Instr Flags(FlagsUpdate S) {
    if (S == SetFlags) {
      return 1 << FlagsUpdate_offset;
    } else if (S == LeaveFlags) {
      return 0 << FlagsUpdate_offset;
    }
    VIXL_UNREACHABLE();
    return 0;
  }

  static Instr Cond(Condition cond) { return cond << Condition_offset; }

  // PC-relative address encoding.
  static Instr ImmPCRelAddress(int64_t imm21) {
    VIXL_ASSERT(IsInt21(imm21));
    Instr imm = static_cast<Instr>(TruncateToUint21(imm21));
    Instr immhi = (imm >> ImmPCRelLo_width) << ImmPCRelHi_offset;
    Instr immlo = imm << ImmPCRelLo_offset;
    return (immhi & ImmPCRelHi_mask) | (immlo & ImmPCRelLo_mask);
  }

  // Branch encoding.
  static Instr ImmUncondBranch(int64_t imm26) {
    VIXL_ASSERT(IsInt26(imm26));
    return TruncateToUint26(imm26) << ImmUncondBranch_offset;
  }

  static Instr ImmCondBranch(int64_t imm19) {
    VIXL_ASSERT(IsInt19(imm19));
    return TruncateToUint19(imm19) << ImmCondBranch_offset;
  }

  static Instr ImmCmpBranch(int64_t imm19) {
    VIXL_ASSERT(IsInt19(imm19));
    return TruncateToUint19(imm19) << ImmCmpBranch_offset;
  }

  static Instr ImmTestBranch(int64_t imm14) {
    VIXL_ASSERT(IsInt14(imm14));
    return TruncateToUint14(imm14) << ImmTestBranch_offset;
  }

  static Instr ImmTestBranchBit(unsigned bit_pos) {
    VIXL_ASSERT(IsUint6(bit_pos));
    // Subtract five from the shift offset, as we need bit 5 from bit_pos.
    unsigned b5 = bit_pos << (ImmTestBranchBit5_offset - 5);
    unsigned b40 = bit_pos << ImmTestBranchBit40_offset;
    b5 &= ImmTestBranchBit5_mask;
    b40 &= ImmTestBranchBit40_mask;
    return b5 | b40;
  }

  // Data Processing encoding.
  static Instr SF(Register rd) {
    return rd.Is64Bits() ? SixtyFourBits : ThirtyTwoBits;
  }

  static Instr ImmAddSub(int imm) {
    VIXL_ASSERT(IsImmAddSub(imm));
    if (IsUint12(imm)) {  // No shift required.
      imm <<= ImmAddSub_offset;
    } else {
      imm = ((imm >> 12) << ImmAddSub_offset) | (1 << ShiftAddSub_offset);
    }
    return imm;
  }

  static Instr ImmS(unsigned imms, unsigned reg_size) {
    VIXL_ASSERT(((reg_size == kXRegSize) && IsUint6(imms)) ||
                ((reg_size == kWRegSize) && IsUint5(imms)));
    USE(reg_size);
    return imms << ImmS_offset;
  }

  static Instr ImmR(unsigned immr, unsigned reg_size) {
    VIXL_ASSERT(((reg_size == kXRegSize) && IsUint6(immr)) ||
                ((reg_size == kWRegSize) && IsUint5(immr)));
    USE(reg_size);
    VIXL_ASSERT(IsUint6(immr));
    return immr << ImmR_offset;
  }

  static Instr ImmSetBits(unsigned imms, unsigned reg_size) {
    VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
    VIXL_ASSERT(IsUint6(imms));
    VIXL_ASSERT((reg_size == kXRegSize) || IsUint6(imms + 3));
    USE(reg_size);
    return imms << ImmSetBits_offset;
  }

  static Instr ImmRotate(unsigned immr, unsigned reg_size) {
    VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
    VIXL_ASSERT(((reg_size == kXRegSize) && IsUint6(immr)) ||
                ((reg_size == kWRegSize) && IsUint5(immr)));
    USE(reg_size);
    return immr << ImmRotate_offset;
  }

  static Instr ImmLLiteral(int64_t imm19) {
    VIXL_ASSERT(IsInt19(imm19));
    return TruncateToUint19(imm19) << ImmLLiteral_offset;
  }

  static Instr BitN(unsigned bitn, unsigned reg_size) {
    VIXL_ASSERT((reg_size == kWRegSize) || (reg_size == kXRegSize));
    VIXL_ASSERT((reg_size == kXRegSize) || (bitn == 0));
    USE(reg_size);
    return bitn << BitN_offset;
  }

  static Instr ShiftDP(Shift shift) {
    VIXL_ASSERT(shift == LSL || shift == LSR || shift == ASR || shift == ROR);
    return shift << ShiftDP_offset;
  }

  static Instr ImmDPShift(unsigned amount) {
    VIXL_ASSERT(IsUint6(amount));
    return amount << ImmDPShift_offset;
  }

  static Instr ExtendMode(Extend extend) { return extend << ExtendMode_offset; }

  static Instr ImmExtendShift(unsigned left_shift) {
    VIXL_ASSERT(left_shift <= 4);
    return left_shift << ImmExtendShift_offset;
  }

  static Instr ImmCondCmp(unsigned imm) {
    VIXL_ASSERT(IsUint5(imm));
    return imm << ImmCondCmp_offset;
  }

  static Instr Nzcv(StatusFlags nzcv) {
    return ((nzcv >> Flags_offset) & 0xf) << Nzcv_offset;
  }

  // MemOperand offset encoding.
  static Instr ImmLSUnsigned(int64_t imm12) {
    VIXL_ASSERT(IsUint12(imm12));
    return TruncateToUint12(imm12) << ImmLSUnsigned_offset;
  }

  static Instr ImmLS(int64_t imm9) {
    VIXL_ASSERT(IsInt9(imm9));
    return TruncateToUint9(imm9) << ImmLS_offset;
  }

  static Instr ImmLSPair(int64_t imm7, unsigned access_size) {
    VIXL_ASSERT(IsMultiple(imm7, 1 << access_size));
    int64_t scaled_imm7 = imm7 / (1 << access_size);
    VIXL_ASSERT(IsInt7(scaled_imm7));
    return TruncateToUint7(scaled_imm7) << ImmLSPair_offset;
  }

  static Instr ImmShiftLS(unsigned shift_amount) {
    VIXL_ASSERT(IsUint1(shift_amount));
    return shift_amount << ImmShiftLS_offset;
  }

  static Instr ImmPrefetchOperation(int imm5) {
    VIXL_ASSERT(IsUint5(imm5));
    return imm5 << ImmPrefetchOperation_offset;
  }

  static Instr ImmException(int imm16) {
    VIXL_ASSERT(IsUint16(imm16));
    return imm16 << ImmException_offset;
  }

  static Instr ImmSystemRegister(int imm16) {
    VIXL_ASSERT(IsUint16(imm16));
    return imm16 << ImmSystemRegister_offset;
  }

  static Instr ImmHint(int imm7) {
    VIXL_ASSERT(IsUint7(imm7));
    return imm7 << ImmHint_offset;
  }

  static Instr CRm(int imm4) {
    VIXL_ASSERT(IsUint4(imm4));
    return imm4 << CRm_offset;
  }

  static Instr CRn(int imm4) {
    VIXL_ASSERT(IsUint4(imm4));
    return imm4 << CRn_offset;
  }

  static Instr SysOp(int imm14) {
    VIXL_ASSERT(IsUint14(imm14));
    return imm14 << SysOp_offset;
  }

  static Instr ImmSysOp1(int imm3) {
    VIXL_ASSERT(IsUint3(imm3));
    return imm3 << SysOp1_offset;
  }

  static Instr ImmSysOp2(int imm3) {
    VIXL_ASSERT(IsUint3(imm3));
    return imm3 << SysOp2_offset;
  }

  static Instr ImmBarrierDomain(int imm2) {
    VIXL_ASSERT(IsUint2(imm2));
    return imm2 << ImmBarrierDomain_offset;
  }

  static Instr ImmBarrierType(int imm2) {
    VIXL_ASSERT(IsUint2(imm2));
    return imm2 << ImmBarrierType_offset;
  }

  // Move immediates encoding.
  static Instr ImmMoveWide(uint64_t imm) {
    VIXL_ASSERT(IsUint16(imm));
    return static_cast<Instr>(imm << ImmMoveWide_offset);
  }

  static Instr ShiftMoveWide(int64_t shift) {
    VIXL_ASSERT(IsUint2(shift));
    return static_cast<Instr>(shift << ShiftMoveWide_offset);
  }

  // FP Immediates.
  static Instr ImmFP16(Float16 imm);
  static Instr ImmFP32(float imm);
  static Instr ImmFP64(double imm);

  // FP register type.
  static Instr FPType(FPRegister fd) {
    switch (fd.GetSizeInBits()) {
      case 16:
        return FP16;
      case 32:
        return FP32;
      case 64:
        return FP64;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }
  }

  static Instr FPScale(unsigned scale) {
    VIXL_ASSERT(IsUint6(scale));
    return scale << FPScale_offset;
  }

  // Immediate field checking helpers.
  static bool IsImmAddSub(int64_t immediate);
  static bool IsImmConditionalCompare(int64_t immediate);
  static bool IsImmFP16(Float16 imm);
  static bool IsImmFP32(float imm);
  static bool IsImmFP64(double imm);
  static bool IsImmLogical(uint64_t value,
                           unsigned width,
                           unsigned* n = NULL,
                           unsigned* imm_s = NULL,
                           unsigned* imm_r = NULL);
  static bool IsImmLSPair(int64_t offset, unsigned access_size);
  static bool IsImmLSScaled(int64_t offset, unsigned access_size);
  static bool IsImmLSUnscaled(int64_t offset);
  static bool IsImmMovn(uint64_t imm, unsigned reg_size);
  static bool IsImmMovz(uint64_t imm, unsigned reg_size);

  // Instruction bits for vector format in data processing operations.
  static Instr VFormat(VRegister vd) {
    if (vd.Is64Bits()) {
      switch (vd.GetLanes()) {
        case 2:
          return NEON_2S;
        case 4:
          return NEON_4H;
        case 8:
          return NEON_8B;
        default:
          return 0xffffffff;
      }
    } else {
      VIXL_ASSERT(vd.Is128Bits());
      switch (vd.GetLanes()) {
        case 2:
          return NEON_2D;
        case 4:
          return NEON_4S;
        case 8:
          return NEON_8H;
        case 16:
          return NEON_16B;
        default:
          return 0xffffffff;
      }
    }
  }

  // Instruction bits for vector format in floating point data processing
  // operations.
  static Instr FPFormat(VRegister vd) {
    switch (vd.GetLanes()) {
      case 1:
        // Floating point scalar formats.
        switch (vd.GetSizeInBits()) {
          case 16:
            return FP16;
          case 32:
            return FP32;
          case 64:
            return FP64;
          default:
            VIXL_UNREACHABLE();
        }
        break;
      case 2:
        // Two lane floating point vector formats.
        switch (vd.GetSizeInBits()) {
          case 64:
            return NEON_FP_2S;
          case 128:
            return NEON_FP_2D;
          default:
            VIXL_UNREACHABLE();
        }
        break;
      case 4:
        // Four lane floating point vector formats.
        switch (vd.GetSizeInBits()) {
          case 64:
            return NEON_FP_4H;
          case 128:
            return NEON_FP_4S;
          default:
            VIXL_UNREACHABLE();
        }
        break;
      case 8:
        // Eight lane floating point vector format.
        VIXL_ASSERT(vd.Is128Bits());
        return NEON_FP_8H;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }
    VIXL_UNREACHABLE();
    return 0;
  }

  // Instruction bits for vector format in load and store operations.
  static Instr LSVFormat(VRegister vd) {
    if (vd.Is64Bits()) {
      switch (vd.GetLanes()) {
        case 1:
          return LS_NEON_1D;
        case 2:
          return LS_NEON_2S;
        case 4:
          return LS_NEON_4H;
        case 8:
          return LS_NEON_8B;
        default:
          return 0xffffffff;
      }
    } else {
      VIXL_ASSERT(vd.Is128Bits());
      switch (vd.GetLanes()) {
        case 2:
          return LS_NEON_2D;
        case 4:
          return LS_NEON_4S;
        case 8:
          return LS_NEON_8H;
        case 16:
          return LS_NEON_16B;
        default:
          return 0xffffffff;
      }
    }
  }

  // Instruction bits for scalar format in data processing operations.
  static Instr SFormat(VRegister vd) {
    VIXL_ASSERT(vd.GetLanes() == 1);
    switch (vd.GetSizeInBytes()) {
      case 1:
        return NEON_B;
      case 2:
        return NEON_H;
      case 4:
        return NEON_S;
      case 8:
        return NEON_D;
      default:
        return 0xffffffff;
    }
  }

  static Instr ImmNEONHLM(int index, int num_bits) {
    int h, l, m;
    if (num_bits == 3) {
      VIXL_ASSERT(IsUint3(index));
      h = (index >> 2) & 1;
      l = (index >> 1) & 1;
      m = (index >> 0) & 1;
    } else if (num_bits == 2) {
      VIXL_ASSERT(IsUint2(index));
      h = (index >> 1) & 1;
      l = (index >> 0) & 1;
      m = 0;
    } else {
      VIXL_ASSERT(IsUint1(index) && (num_bits == 1));
      h = (index >> 0) & 1;
      l = 0;
      m = 0;
    }
    return (h << NEONH_offset) | (l << NEONL_offset) | (m << NEONM_offset);
  }

  static Instr ImmRotFcadd(int rot) {
    VIXL_ASSERT(rot == 90 || rot == 270);
    return (((rot == 270) ? 1 : 0) << ImmRotFcadd_offset);
  }

  static Instr ImmRotFcmlaSca(int rot) {
    VIXL_ASSERT(rot == 0 || rot == 90 || rot == 180 || rot == 270);
    return (rot / 90) << ImmRotFcmlaSca_offset;
  }

  static Instr ImmRotFcmlaVec(int rot) {
    VIXL_ASSERT(rot == 0 || rot == 90 || rot == 180 || rot == 270);
    return (rot / 90) << ImmRotFcmlaVec_offset;
  }

  static Instr ImmNEONExt(int imm4) {
    VIXL_ASSERT(IsUint4(imm4));
    return imm4 << ImmNEONExt_offset;
  }

  static Instr ImmNEON5(Instr format, int index) {
    VIXL_ASSERT(IsUint4(index));
    int s = LaneSizeInBytesLog2FromFormat(static_cast<VectorFormat>(format));
    int imm5 = (index << (s + 1)) | (1 << s);
    return imm5 << ImmNEON5_offset;
  }

  static Instr ImmNEON4(Instr format, int index) {
    VIXL_ASSERT(IsUint4(index));
    int s = LaneSizeInBytesLog2FromFormat(static_cast<VectorFormat>(format));
    int imm4 = index << s;
    return imm4 << ImmNEON4_offset;
  }

  static Instr ImmNEONabcdefgh(int imm8) {
    VIXL_ASSERT(IsUint8(imm8));
    Instr instr;
    instr = ((imm8 >> 5) & 7) << ImmNEONabc_offset;
    instr |= (imm8 & 0x1f) << ImmNEONdefgh_offset;
    return instr;
  }

  static Instr NEONCmode(int cmode) {
    VIXL_ASSERT(IsUint4(cmode));
    return cmode << NEONCmode_offset;
  }

  static Instr NEONModImmOp(int op) {
    VIXL_ASSERT(IsUint1(op));
    return op << NEONModImmOp_offset;
  }

  // Size of the code generated since label to the current position.
  size_t GetSizeOfCodeGeneratedSince(Label* label) const {
    VIXL_ASSERT(label->IsBound());
    return GetBuffer().GetOffsetFrom(label->GetLocation());
  }
  VIXL_DEPRECATED("GetSizeOfCodeGeneratedSince",
                  size_t SizeOfCodeGeneratedSince(Label* label) const) {
    return GetSizeOfCodeGeneratedSince(label);
  }

  VIXL_DEPRECATED("GetBuffer().GetCapacity()",
                  size_t GetBufferCapacity() const) {
    return GetBuffer().GetCapacity();
  }
  VIXL_DEPRECATED("GetBuffer().GetCapacity()", size_t BufferCapacity() const) {
    return GetBuffer().GetCapacity();
  }

  VIXL_DEPRECATED("GetBuffer().GetRemainingBytes()",
                  size_t GetRemainingBufferSpace() const) {
    return GetBuffer().GetRemainingBytes();
  }
  VIXL_DEPRECATED("GetBuffer().GetRemainingBytes()",
                  size_t RemainingBufferSpace() const) {
    return GetBuffer().GetRemainingBytes();
  }

  PositionIndependentCodeOption GetPic() const { return pic_; }
  VIXL_DEPRECATED("GetPic", PositionIndependentCodeOption pic() const) {
    return GetPic();
  }

  CPUFeatures* GetCPUFeatures() { return &cpu_features_; }

  void SetCPUFeatures(const CPUFeatures& cpu_features) {
    cpu_features_ = cpu_features;
  }

  bool AllowPageOffsetDependentCode() const {
    return (GetPic() == PageOffsetDependentCode) ||
           (GetPic() == PositionDependentCode);
  }

  static Register AppropriateZeroRegFor(const CPURegister& reg) {
    return reg.Is64Bits() ? Register(xzr) : Register(wzr);
  }

 protected:
  void LoadStore(const CPURegister& rt,
                 const MemOperand& addr,
                 LoadStoreOp op,
                 LoadStoreScalingOption option = PreferScaledOffset);

  void LoadStorePair(const CPURegister& rt,
                     const CPURegister& rt2,
                     const MemOperand& addr,
                     LoadStorePairOp op);
  void LoadStoreStruct(const VRegister& vt,
                       const MemOperand& addr,
                       NEONLoadStoreMultiStructOp op);
  void LoadStoreStruct1(const VRegister& vt,
                        int reg_count,
                        const MemOperand& addr);
  void LoadStoreStructSingle(const VRegister& vt,
                             uint32_t lane,
                             const MemOperand& addr,
                             NEONLoadStoreSingleStructOp op);
  void LoadStoreStructSingleAllLanes(const VRegister& vt,
                                     const MemOperand& addr,
                                     NEONLoadStoreSingleStructOp op);
  void LoadStoreStructVerify(const VRegister& vt,
                             const MemOperand& addr,
                             Instr op);

  void Prefetch(PrefetchOperation op,
                const MemOperand& addr,
                LoadStoreScalingOption option = PreferScaledOffset);

  // TODO(all): The third parameter should be passed by reference but gcc 4.8.2
  // reports a bogus uninitialised warning then.
  void Logical(const Register& rd,
               const Register& rn,
               const Operand operand,
               LogicalOp op);
  void LogicalImmediate(const Register& rd,
                        const Register& rn,
                        unsigned n,
                        unsigned imm_s,
                        unsigned imm_r,
                        LogicalOp op);

  void ConditionalCompare(const Register& rn,
                          const Operand& operand,
                          StatusFlags nzcv,
                          Condition cond,
                          ConditionalCompareOp op);

  void AddSubWithCarry(const Register& rd,
                       const Register& rn,
                       const Operand& operand,
                       FlagsUpdate S,
                       AddSubWithCarryOp op);


  // Functions for emulating operands not directly supported by the instruction
  // set.
  void EmitShift(const Register& rd,
                 const Register& rn,
                 Shift shift,
                 unsigned amount);
  void EmitExtendShift(const Register& rd,
                       const Register& rn,
                       Extend extend,
                       unsigned left_shift);

  void AddSub(const Register& rd,
              const Register& rn,
              const Operand& operand,
              FlagsUpdate S,
              AddSubOp op);

  void NEONTable(const VRegister& vd,
                 const VRegister& vn,
                 const VRegister& vm,
                 NEONTableOp op);

  // Find an appropriate LoadStoreOp or LoadStorePairOp for the specified
  // registers. Only simple loads are supported; sign- and zero-extension (such
  // as in LDPSW_x or LDRB_w) are not supported.
  static LoadStoreOp LoadOpFor(const CPURegister& rt);
  static LoadStorePairOp LoadPairOpFor(const CPURegister& rt,
                                       const CPURegister& rt2);
  static LoadStoreOp StoreOpFor(const CPURegister& rt);
  static LoadStorePairOp StorePairOpFor(const CPURegister& rt,
                                        const CPURegister& rt2);
  static LoadStorePairNonTemporalOp LoadPairNonTemporalOpFor(
      const CPURegister& rt, const CPURegister& rt2);
  static LoadStorePairNonTemporalOp StorePairNonTemporalOpFor(
      const CPURegister& rt, const CPURegister& rt2);
  static LoadLiteralOp LoadLiteralOpFor(const CPURegister& rt);

  // Convenience pass-through for CPU feature checks.
  bool CPUHas(CPUFeatures::Feature feature0,
              CPUFeatures::Feature feature1 = CPUFeatures::kNone,
              CPUFeatures::Feature feature2 = CPUFeatures::kNone,
              CPUFeatures::Feature feature3 = CPUFeatures::kNone) const {
    return cpu_features_.Has(feature0, feature1, feature2, feature3);
  }

  // Determine whether the target CPU has the specified registers, based on the
  // currently-enabled CPU features. Presence of a register does not imply
  // support for arbitrary operations on it. For example, CPUs with FP have H
  // registers, but most half-precision operations require the FPHalf feature.
  //
  // These are used to check CPU features in loads and stores that have the same
  // entry point for both integer and FP registers.
  bool CPUHas(const CPURegister& rt) const;
  bool CPUHas(const CPURegister& rt, const CPURegister& rt2) const;

 private:
  static uint32_t FP16ToImm8(Float16 imm);
  static uint32_t FP32ToImm8(float imm);
  static uint32_t FP64ToImm8(double imm);

  // Instruction helpers.
  void MoveWide(const Register& rd,
                uint64_t imm,
                int shift,
                MoveWideImmediateOp mov_op);
  void DataProcShiftedRegister(const Register& rd,
                               const Register& rn,
                               const Operand& operand,
                               FlagsUpdate S,
                               Instr op);
  void DataProcExtendedRegister(const Register& rd,
                                const Register& rn,
                                const Operand& operand,
                                FlagsUpdate S,
                                Instr op);
  void LoadStorePairNonTemporal(const CPURegister& rt,
                                const CPURegister& rt2,
                                const MemOperand& addr,
                                LoadStorePairNonTemporalOp op);
  void LoadLiteral(const CPURegister& rt, uint64_t imm, LoadLiteralOp op);
  void ConditionalSelect(const Register& rd,
                         const Register& rn,
                         const Register& rm,
                         Condition cond,
                         ConditionalSelectOp op);
  void DataProcessing1Source(const Register& rd,
                             const Register& rn,
                             DataProcessing1SourceOp op);
  void DataProcessing3Source(const Register& rd,
                             const Register& rn,
                             const Register& rm,
                             const Register& ra,
                             DataProcessing3SourceOp op);
  void FPDataProcessing1Source(const VRegister& fd,
                               const VRegister& fn,
                               FPDataProcessing1SourceOp op);
  void FPDataProcessing3Source(const VRegister& fd,
                               const VRegister& fn,
                               const VRegister& fm,
                               const VRegister& fa,
                               FPDataProcessing3SourceOp op);
  void NEONAcrossLanesL(const VRegister& vd,
                        const VRegister& vn,
                        NEONAcrossLanesOp op);
  void NEONAcrossLanes(const VRegister& vd,
                       const VRegister& vn,
                       NEONAcrossLanesOp op,
                       Instr op_half);
  void NEONModifiedImmShiftLsl(const VRegister& vd,
                               const int imm8,
                               const int left_shift,
                               NEONModifiedImmediateOp op);
  void NEONModifiedImmShiftMsl(const VRegister& vd,
                               const int imm8,
                               const int shift_amount,
                               NEONModifiedImmediateOp op);
  void NEONFP2Same(const VRegister& vd, const VRegister& vn, Instr vop);
  void NEON3Same(const VRegister& vd,
                 const VRegister& vn,
                 const VRegister& vm,
                 NEON3SameOp vop);
  void NEON3SameFP16(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm,
                     Instr op);
  void NEONFP3Same(const VRegister& vd,
                   const VRegister& vn,
                   const VRegister& vm,
                   Instr op);
  void NEON3DifferentL(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       NEON3DifferentOp vop);
  void NEON3DifferentW(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       NEON3DifferentOp vop);
  void NEON3DifferentHN(const VRegister& vd,
                        const VRegister& vn,
                        const VRegister& vm,
                        NEON3DifferentOp vop);
  void NEONFP2RegMisc(const VRegister& vd,
                      const VRegister& vn,
                      NEON2RegMiscOp vop,
                      double value = 0.0);
  void NEONFP2RegMiscFP16(const VRegister& vd,
                          const VRegister& vn,
                          NEON2RegMiscFP16Op vop,
                          double value = 0.0);
  void NEON2RegMisc(const VRegister& vd,
                    const VRegister& vn,
                    NEON2RegMiscOp vop,
                    int value = 0);
  void NEONFP2RegMisc(const VRegister& vd, const VRegister& vn, Instr op);
  void NEONFP2RegMiscFP16(const VRegister& vd, const VRegister& vn, Instr op);
  void NEONAddlp(const VRegister& vd, const VRegister& vn, NEON2RegMiscOp op);
  void NEONPerm(const VRegister& vd,
                const VRegister& vn,
                const VRegister& vm,
                NEONPermOp op);
  void NEONFPByElement(const VRegister& vd,
                       const VRegister& vn,
                       const VRegister& vm,
                       int vm_index,
                       NEONByIndexedElementOp op,
                       NEONByIndexedElementOp op_half);
  void NEONByElement(const VRegister& vd,
                     const VRegister& vn,
                     const VRegister& vm,
                     int vm_index,
                     NEONByIndexedElementOp op);
  void NEONByElementL(const VRegister& vd,
                      const VRegister& vn,
                      const VRegister& vm,
                      int vm_index,
                      NEONByIndexedElementOp op);
  void NEONShiftImmediate(const VRegister& vd,
                          const VRegister& vn,
                          NEONShiftImmediateOp op,
                          int immh_immb);
  void NEONShiftLeftImmediate(const VRegister& vd,
                              const VRegister& vn,
                              int shift,
                              NEONShiftImmediateOp op);
  void NEONShiftRightImmediate(const VRegister& vd,
                               const VRegister& vn,
                               int shift,
                               NEONShiftImmediateOp op);
  void NEONShiftImmediateL(const VRegister& vd,
                           const VRegister& vn,
                           int shift,
                           NEONShiftImmediateOp op);
  void NEONShiftImmediateN(const VRegister& vd,
                           const VRegister& vn,
                           int shift,
                           NEONShiftImmediateOp op);
  void NEONXtn(const VRegister& vd, const VRegister& vn, NEON2RegMiscOp vop);

  Instr LoadStoreStructAddrModeField(const MemOperand& addr);

  // Encode the specified MemOperand for the specified access size and scaling
  // preference.
  Instr LoadStoreMemOperand(const MemOperand& addr,
                            unsigned access_size,
                            LoadStoreScalingOption option);

  // Link the current (not-yet-emitted) instruction to the specified label, then
  // return an offset to be encoded in the instruction. If the label is not yet
  // bound, an offset of 0 is returned.
  ptrdiff_t LinkAndGetByteOffsetTo(Label* label);
  ptrdiff_t LinkAndGetInstructionOffsetTo(Label* label);
  ptrdiff_t LinkAndGetPageOffsetTo(Label* label);

  // A common implementation for the LinkAndGet<Type>OffsetTo helpers.
  template <int element_shift>
  ptrdiff_t LinkAndGetOffsetTo(Label* label);

  // Literal load offset are in words (32-bit).
  ptrdiff_t LinkAndGetWordOffsetTo(RawLiteral* literal);

  // Emit the instruction in buffer_.
  void Emit(Instr instruction) {
    VIXL_STATIC_ASSERT(sizeof(instruction) == kInstructionSize);
    VIXL_ASSERT(AllowAssembler());
    GetBuffer()->Emit32(instruction);
  }

  PositionIndependentCodeOption pic_;

  CPUFeatures cpu_features_;
};


template <typename T>
void Literal<T>::UpdateValue(T new_value, const Assembler* assembler) {
  return UpdateValue(new_value,
                     assembler->GetBuffer().GetStartAddress<uint8_t*>());
}


template <typename T>
void Literal<T>::UpdateValue(T high64, T low64, const Assembler* assembler) {
  return UpdateValue(high64,
                     low64,
                     assembler->GetBuffer().GetStartAddress<uint8_t*>());
}


}  // namespace aarch64

// Required InvalSet template specialisations.
// TODO: These template specialisations should not live in this file.  Move
// Label out of the aarch64 namespace in order to share its implementation
// later.
#define INVAL_SET_TEMPLATE_PARAMETERS                                \
  ptrdiff_t, aarch64::Label::kNPreallocatedLinks, ptrdiff_t,         \
      aarch64::Label::kInvalidLinkKey, aarch64::Label::kReclaimFrom, \
      aarch64::Label::kReclaimFactor
template <>
inline ptrdiff_t InvalSet<INVAL_SET_TEMPLATE_PARAMETERS>::GetKey(
    const ptrdiff_t& element) {
  return element;
}
template <>
inline void InvalSet<INVAL_SET_TEMPLATE_PARAMETERS>::SetKey(ptrdiff_t* element,
                                                            ptrdiff_t key) {
  *element = key;
}
#undef INVAL_SET_TEMPLATE_PARAMETERS

}  // namespace vixl

#endif  // VIXL_AARCH64_ASSEMBLER_AARCH64_H_
