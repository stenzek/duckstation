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

#ifndef VIXL_AARCH32_LABEL_AARCH32_H_
#define VIXL_AARCH32_LABEL_AARCH32_H_

extern "C" {
#include <stdint.h>
}

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <list>

#include "invalset-vixl.h"
#include "pool-manager.h"
#include "utils-vixl.h"

#include "constants-aarch32.h"
#include "instructions-aarch32.h"

namespace vixl {

namespace aarch32 {

class MacroAssembler;

class Location : public LocationBase<int32_t> {
  friend class Assembler;
  friend class MacroAssembler;

 public:
  // Unbound location that can be used with the assembler bind() method and
  // with the assembler methods for generating instructions, but will never
  // be handled by the pool manager.
  Location()
      : LocationBase<int32_t>(kRawLocation, 1 /* dummy size*/),
        referenced_(false) {}

  typedef int32_t Offset;

  ~Location() {
#ifdef VIXL_DEBUG
    if (IsReferenced() && !IsBound()) {
      VIXL_ABORT_WITH_MSG("Location, label or literal used but not bound.\n");
    }
#endif
  }

  bool IsReferenced() const { return referenced_; }

 private:
  class EmitOperator {
   public:
    explicit EmitOperator(InstructionSet isa) : isa_(isa) {
#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
      USE(isa_);
      VIXL_ASSERT(isa == A32);
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
      USE(isa_);
      VIXL_ASSERT(isa == T32);
#endif
    }
    virtual ~EmitOperator() {}
    virtual uint32_t Encode(uint32_t /*instr*/,
                            Location::Offset /*pc*/,
                            const Location* /*label*/) const {
      return 0;
    }
#if defined(VIXL_INCLUDE_TARGET_A32_ONLY)
    bool IsUsingT32() const { return false; }
#elif defined(VIXL_INCLUDE_TARGET_T32_ONLY)
    bool IsUsingT32() const { return true; }
#else
    bool IsUsingT32() const { return isa_ == T32; }
#endif

   private:
    InstructionSet isa_;
  };

 protected:
  class ForwardRef : public ForwardReference<int32_t> {
   public:
    // Default constructor for InvalSet.
    ForwardRef() : ForwardReference<int32_t>(0, 0, 0, 0, 1), op_(NULL) {}

    ForwardRef(const Location::EmitOperator* op,
               int32_t location,
               int size,
               int32_t min_object_location,
               int32_t max_object_location,
               int object_alignment = 1)
        : ForwardReference<int32_t>(location,
                                    size,
                                    min_object_location,
                                    max_object_location,
                                    object_alignment),
          op_(op) {}

    const Location::EmitOperator* op() const { return op_; }

    // We must provide comparison operators to work with InvalSet.
    bool operator==(const ForwardRef& other) const {
      return GetLocation() == other.GetLocation();
    }
    bool operator<(const ForwardRef& other) const {
      return GetLocation() < other.GetLocation();
    }
    bool operator<=(const ForwardRef& other) const {
      return GetLocation() <= other.GetLocation();
    }
    bool operator>(const ForwardRef& other) const {
      return GetLocation() > other.GetLocation();
    }

   private:
    const Location::EmitOperator* op_;
  };

  static const int kNPreallocatedElements = 4;
  // The following parameters will not affect ForwardRefList in practice, as we
  // resolve all references at once and clear the list, so we do not need to
  // remove individual elements by invalidating them.
  static const int32_t kInvalidLinkKey = INT32_MAX;
  static const size_t kReclaimFrom = 512;
  static const size_t kReclaimFactor = 2;

  typedef InvalSet<ForwardRef,
                   kNPreallocatedElements,
                   int32_t,
                   kInvalidLinkKey,
                   kReclaimFrom,
                   kReclaimFactor>
      ForwardRefListBase;
  typedef InvalSetIterator<ForwardRefListBase> ForwardRefListIteratorBase;

  class ForwardRefList : public ForwardRefListBase {
   public:
    ForwardRefList() : ForwardRefListBase() {}

    using ForwardRefListBase::Back;
    using ForwardRefListBase::Front;
  };

  class ForwardRefListIterator : public ForwardRefListIteratorBase {
   public:
    explicit ForwardRefListIterator(Location* location)
        : ForwardRefListIteratorBase(&location->forward_) {}

    // TODO: Remove these and use the STL-like interface instead. We'll need a
    // const_iterator implemented for this.
    using ForwardRefListIteratorBase::Advance;
    using ForwardRefListIteratorBase::Current;
  };

  // For InvalSet::GetKey() and InvalSet::SetKey().
  friend class InvalSet<ForwardRef,
                        kNPreallocatedElements,
                        int32_t,
                        kInvalidLinkKey,
                        kReclaimFrom,
                        kReclaimFactor>;

 private:
  virtual void ResolveReferences(internal::AssemblerBase* assembler)
      VIXL_OVERRIDE;

  void SetReferenced() { referenced_ = true; }

  bool HasForwardReferences() const { return !forward_.empty(); }

  ForwardRef GetLastForwardReference() const {
    VIXL_ASSERT(HasForwardReferences());
    return forward_.Back();
  }

  // Add forward reference to this object. Called from the assembler.
  void AddForwardRef(int32_t instr_location,
                     const EmitOperator& op,
                     const ReferenceInfo* info);

  // Check if we need to add padding when binding this object, in order to
  // meet the minimum location requirement.
  bool Needs16BitPadding(int location) const;

  void EncodeLocationFor(internal::AssemblerBase* assembler,
                         int32_t from,
                         const Location::EmitOperator* encoder);

  // True if the label has been used at least once.
  bool referenced_;

 protected:
  // Types passed to LocationBase. Must be distinct for unbound Locations (not
  // relevant for bound locations, as they don't have a correspoding
  // PoolObject).
  static const int kRawLocation = 0;  // Will not be used by the pool manager.
  static const int kVeneerType = 1;
  static const int kLiteralType = 2;

  // Contains the references to the unbound label
  ForwardRefList forward_;

  // To be used only by derived classes.
  Location(uint32_t type, int size, int alignment)
      : LocationBase<int32_t>(type, size, alignment), referenced_(false) {}

  // To be used only by derived classes.
  explicit Location(Offset location)
      : LocationBase<int32_t>(location), referenced_(false) {}

  virtual int GetMaxAlignment() const VIXL_OVERRIDE;
  virtual int GetMinLocation() const VIXL_OVERRIDE;

 private:
  // Included to make the class concrete, however should never be called.
  virtual void EmitPoolObject(MacroAssemblerInterface* masm) VIXL_OVERRIDE {
    USE(masm);
    VIXL_UNREACHABLE();
  }
};

class Label : public Location {
  static const int kVeneerSize = 4;
  // Use an alignment of 1 for all architectures. Even though we can bind an
  // unused label, because of the way the MacroAssembler works we can always be
  // sure to have the correct buffer alignment for the instruction set we are
  // using, so we do not need to enforce additional alignment requirements
  // here.
  // TODO: Consider modifying the interface of the pool manager to pass an
  // optional additional alignment to Bind() in order to handle cases where the
  // buffer could be unaligned.
  static const int kVeneerAlignment = 1;

 public:
  Label() : Location(kVeneerType, kVeneerSize, kVeneerAlignment) {}
  explicit Label(Offset location) : Location(location) {}

 private:
  virtual bool ShouldBeDeletedOnPlacementByPoolManager() const VIXL_OVERRIDE {
    return false;
  }
  virtual bool ShouldDeletePoolObjectOnPlacement() const VIXL_OVERRIDE {
    return false;
  }

  virtual void UpdatePoolObject(PoolObject<int32_t>* object) VIXL_OVERRIDE;
  virtual void EmitPoolObject(MacroAssemblerInterface* masm) VIXL_OVERRIDE;

  virtual bool UsePoolObjectEmissionMargin() const VIXL_OVERRIDE {
    return true;
  }
  virtual int32_t GetPoolObjectEmissionMargin() const VIXL_OVERRIDE {
    VIXL_ASSERT(UsePoolObjectEmissionMargin() == true);
    return 1 * KBytes;
  }
};

class RawLiteral : public Location {
  // Some load instructions require alignment to 4 bytes. Since we do
  // not know what instructions will reference a literal after we place
  // it, we enforce a 4 byte alignment for literals that are 4 bytes or
  // larger.
  static const int kLiteralAlignment = 4;

 public:
  enum PlacementPolicy { kPlacedWhenUsed, kManuallyPlaced };

  enum DeletionPolicy {
    kDeletedOnPlacementByPool,
    kDeletedOnPoolDestruction,
    kManuallyDeleted
  };

  RawLiteral(const void* addr,
             int size,
             PlacementPolicy placement_policy = kPlacedWhenUsed,
             DeletionPolicy deletion_policy = kManuallyDeleted)
      : Location(kLiteralType,
                 size,
                 (size < kLiteralAlignment) ? size : kLiteralAlignment),
        addr_(addr),
        manually_placed_(placement_policy == kManuallyPlaced),
        deletion_policy_(deletion_policy) {
    // We can't have manually placed literals that are not manually deleted.
    VIXL_ASSERT(!IsManuallyPlaced() ||
                (GetDeletionPolicy() == kManuallyDeleted));
  }
  RawLiteral(const void* addr, int size, DeletionPolicy deletion_policy)
      : Location(kLiteralType,
                 size,
                 (size < kLiteralAlignment) ? size : kLiteralAlignment),
        addr_(addr),
        manually_placed_(false),
        deletion_policy_(deletion_policy) {}
  const void* GetDataAddress() const { return addr_; }
  int GetSize() const { return GetPoolObjectSizeInBytes(); }

  bool IsManuallyPlaced() const { return manually_placed_; }

 private:
  DeletionPolicy GetDeletionPolicy() const { return deletion_policy_; }

  virtual bool ShouldBeDeletedOnPlacementByPoolManager() const VIXL_OVERRIDE {
    return GetDeletionPolicy() == kDeletedOnPlacementByPool;
  }
  virtual bool ShouldBeDeletedOnPoolManagerDestruction() const VIXL_OVERRIDE {
    return GetDeletionPolicy() == kDeletedOnPoolDestruction;
  }
  virtual void EmitPoolObject(MacroAssemblerInterface* masm) VIXL_OVERRIDE;

  // Data address before it's moved into the code buffer.
  const void* const addr_;
  // When this flag is true, the label will be placed manually.
  bool manually_placed_;
  // When is the literal to be removed from the memory
  // Can be delete'd when:
  //   moved into the code buffer: kDeletedOnPlacementByPool
  //   the pool is delete'd: kDeletedOnPoolDestruction
  //   or left to the application: kManuallyDeleted.
  DeletionPolicy deletion_policy_;

  friend class MacroAssembler;
};

template <typename T>
class Literal : public RawLiteral {
 public:
  explicit Literal(const T& value,
                   PlacementPolicy placement_policy = kPlacedWhenUsed,
                   DeletionPolicy deletion_policy = kManuallyDeleted)
      : RawLiteral(&value_, sizeof(T), placement_policy, deletion_policy),
        value_(value) {}
  explicit Literal(const T& value, DeletionPolicy deletion_policy)
      : RawLiteral(&value_, sizeof(T), deletion_policy), value_(value) {}
  void UpdateValue(const T& value, CodeBuffer* buffer) {
    value_ = value;
    if (IsBound()) {
      buffer->UpdateData(GetLocation(), GetDataAddress(), GetSize());
    }
  }

 private:
  T value_;
};

class StringLiteral : public RawLiteral {
 public:
  explicit StringLiteral(const char* str,
                         PlacementPolicy placement_policy = kPlacedWhenUsed,
                         DeletionPolicy deletion_policy = kManuallyDeleted)
      : RawLiteral(str,
                   static_cast<int>(strlen(str) + 1),
                   placement_policy,
                   deletion_policy) {
    VIXL_ASSERT((strlen(str) + 1) <= kMaxObjectSize);
  }
  explicit StringLiteral(const char* str, DeletionPolicy deletion_policy)
      : RawLiteral(str, static_cast<int>(strlen(str) + 1), deletion_policy) {
    VIXL_ASSERT((strlen(str) + 1) <= kMaxObjectSize);
  }
};

}  // namespace aarch32


// Required InvalSet template specialisations.
#define INVAL_SET_TEMPLATE_PARAMETERS                                       \
  aarch32::Location::ForwardRef, aarch32::Location::kNPreallocatedElements, \
      int32_t, aarch32::Location::kInvalidLinkKey,                          \
      aarch32::Location::kReclaimFrom, aarch32::Location::kReclaimFactor
template <>
inline int32_t InvalSet<INVAL_SET_TEMPLATE_PARAMETERS>::GetKey(
    const aarch32::Location::ForwardRef& element) {
  return element.GetLocation();
}
template <>
inline void InvalSet<INVAL_SET_TEMPLATE_PARAMETERS>::SetKey(
    aarch32::Location::ForwardRef* element, int32_t key) {
  element->SetLocationToInvalidateOnly(key);
}
#undef INVAL_SET_TEMPLATE_PARAMETERS

}  // namespace vixl

#endif  // VIXL_AARCH32_LABEL_AARCH32_H_
