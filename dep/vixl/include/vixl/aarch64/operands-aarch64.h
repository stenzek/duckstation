// Copyright 2016, VIXL authors
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

#ifndef VIXL_AARCH64_OPERANDS_AARCH64_H_
#define VIXL_AARCH64_OPERANDS_AARCH64_H_

#include "instructions-aarch64.h"

namespace vixl {
namespace aarch64 {

typedef uint64_t RegList;
static const int kRegListSizeInBits = sizeof(RegList) * 8;


// Registers.

// Some CPURegister methods can return Register or VRegister types, so we need
// to declare them in advance.
class Register;
class VRegister;

class CPURegister {
 public:
  enum RegisterType {
    // The kInvalid value is used to detect uninitialized static instances,
    // which are always zero-initialized before any constructors are called.
    kInvalid = 0,
    kRegister,
    kVRegister,
    kFPRegister = kVRegister,
    kNoRegister
  };

  CPURegister() : code_(0), size_(0), type_(kNoRegister) {
    VIXL_ASSERT(!IsValid());
    VIXL_ASSERT(IsNone());
  }

  CPURegister(unsigned code, unsigned size, RegisterType type)
      : code_(code), size_(size), type_(type) {
    VIXL_ASSERT(IsValidOrNone());
  }

  unsigned GetCode() const {
    VIXL_ASSERT(IsValid());
    return code_;
  }
  VIXL_DEPRECATED("GetCode", unsigned code() const) { return GetCode(); }

  RegisterType GetType() const {
    VIXL_ASSERT(IsValidOrNone());
    return type_;
  }
  VIXL_DEPRECATED("GetType", RegisterType type() const) { return GetType(); }

  RegList GetBit() const {
    VIXL_ASSERT(code_ < (sizeof(RegList) * 8));
    return IsValid() ? (static_cast<RegList>(1) << code_) : 0;
  }
  VIXL_DEPRECATED("GetBit", RegList Bit() const) { return GetBit(); }

  int GetSizeInBytes() const {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(size_ % 8 == 0);
    return size_ / 8;
  }
  VIXL_DEPRECATED("GetSizeInBytes", int SizeInBytes() const) {
    return GetSizeInBytes();
  }

  int GetSizeInBits() const {
    VIXL_ASSERT(IsValid());
    return size_;
  }
  VIXL_DEPRECATED("GetSizeInBits", unsigned size() const) {
    return GetSizeInBits();
  }
  VIXL_DEPRECATED("GetSizeInBits", int SizeInBits() const) {
    return GetSizeInBits();
  }

  bool Is8Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 8;
  }

  bool Is16Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 16;
  }

  bool Is32Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 32;
  }

  bool Is64Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 64;
  }

  bool Is128Bits() const {
    VIXL_ASSERT(IsValid());
    return size_ == 128;
  }

  bool IsValid() const {
    if (IsValidRegister() || IsValidVRegister()) {
      VIXL_ASSERT(!IsNone());
      return true;
    } else {
      // This assert is hit when the register has not been properly initialized.
      // One cause for this can be an initialisation order fiasco. See
      // https://isocpp.org/wiki/faq/ctors#static-init-order for some details.
      VIXL_ASSERT(IsNone());
      return false;
    }
  }

  bool IsValidRegister() const {
    return IsRegister() && ((size_ == kWRegSize) || (size_ == kXRegSize)) &&
           ((code_ < kNumberOfRegisters) || (code_ == kSPRegInternalCode));
  }

  bool IsValidVRegister() const {
    return IsVRegister() && ((size_ == kBRegSize) || (size_ == kHRegSize) ||
                             (size_ == kSRegSize) || (size_ == kDRegSize) ||
                             (size_ == kQRegSize)) &&
           (code_ < kNumberOfVRegisters);
  }

  bool IsValidFPRegister() const {
    return IsFPRegister() && (code_ < kNumberOfVRegisters);
  }

  bool IsNone() const {
    // kNoRegister types should always have size 0 and code 0.
    VIXL_ASSERT((type_ != kNoRegister) || (code_ == 0));
    VIXL_ASSERT((type_ != kNoRegister) || (size_ == 0));

    return type_ == kNoRegister;
  }

  bool Aliases(const CPURegister& other) const {
    VIXL_ASSERT(IsValidOrNone() && other.IsValidOrNone());
    return (code_ == other.code_) && (type_ == other.type_);
  }

  bool Is(const CPURegister& other) const {
    VIXL_ASSERT(IsValidOrNone() && other.IsValidOrNone());
    return Aliases(other) && (size_ == other.size_);
  }

  bool IsZero() const {
    VIXL_ASSERT(IsValid());
    return IsRegister() && (code_ == kZeroRegCode);
  }

  bool IsSP() const {
    VIXL_ASSERT(IsValid());
    return IsRegister() && (code_ == kSPRegInternalCode);
  }

  bool IsRegister() const { return type_ == kRegister; }

  bool IsVRegister() const { return type_ == kVRegister; }

  bool IsFPRegister() const { return IsS() || IsD(); }

  bool IsW() const { return IsValidRegister() && Is32Bits(); }
  bool IsX() const { return IsValidRegister() && Is64Bits(); }

  // These assertions ensure that the size and type of the register are as
  // described. They do not consider the number of lanes that make up a vector.
  // So, for example, Is8B() implies IsD(), and Is1D() implies IsD, but IsD()
  // does not imply Is1D() or Is8B().
  // Check the number of lanes, ie. the format of the vector, using methods such
  // as Is8B(), Is1D(), etc. in the VRegister class.
  bool IsV() const { return IsVRegister(); }
  bool IsB() const { return IsV() && Is8Bits(); }
  bool IsH() const { return IsV() && Is16Bits(); }
  bool IsS() const { return IsV() && Is32Bits(); }
  bool IsD() const { return IsV() && Is64Bits(); }
  bool IsQ() const { return IsV() && Is128Bits(); }

  // Semantic type for sdot and udot instructions.
  bool IsS4B() const { return IsS(); }
  const VRegister& S4B() const { return S(); }

  const Register& W() const;
  const Register& X() const;
  const VRegister& V() const;
  const VRegister& B() const;
  const VRegister& H() const;
  const VRegister& S() const;
  const VRegister& D() const;
  const VRegister& Q() const;

  bool IsSameType(const CPURegister& other) const {
    return type_ == other.type_;
  }

  bool IsSameSizeAndType(const CPURegister& other) const {
    return (size_ == other.size_) && IsSameType(other);
  }

 protected:
  unsigned code_;
  int size_;
  RegisterType type_;

 private:
  bool IsValidOrNone() const { return IsValid() || IsNone(); }
};


class Register : public CPURegister {
 public:
  Register() : CPURegister() {}
  explicit Register(const CPURegister& other)
      : CPURegister(other.GetCode(), other.GetSizeInBits(), other.GetType()) {
    VIXL_ASSERT(IsValidRegister());
  }
  Register(unsigned code, unsigned size) : CPURegister(code, size, kRegister) {}

  bool IsValid() const {
    VIXL_ASSERT(IsRegister() || IsNone());
    return IsValidRegister();
  }

  static const Register& GetWRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetWRegFromCode",
                  static const Register& WRegFromCode(unsigned code)) {
    return GetWRegFromCode(code);
  }

  static const Register& GetXRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetXRegFromCode",
                  static const Register& XRegFromCode(unsigned code)) {
    return GetXRegFromCode(code);
  }

 private:
  static const Register wregisters[];
  static const Register xregisters[];
};


namespace internal {

template <int size_in_bits>
class FixedSizeRegister : public Register {
 public:
  FixedSizeRegister() : Register() {}
  explicit FixedSizeRegister(unsigned code) : Register(code, size_in_bits) {
    VIXL_ASSERT(IsValidRegister());
  }
  explicit FixedSizeRegister(const Register& other)
      : Register(other.GetCode(), size_in_bits) {
    VIXL_ASSERT(other.GetSizeInBits() == size_in_bits);
    VIXL_ASSERT(IsValidRegister());
  }
  explicit FixedSizeRegister(const CPURegister& other)
      : Register(other.GetCode(), other.GetSizeInBits()) {
    VIXL_ASSERT(other.GetType() == kRegister);
    VIXL_ASSERT(other.GetSizeInBits() == size_in_bits);
    VIXL_ASSERT(IsValidRegister());
  }

  bool IsValid() const {
    return Register::IsValid() && (GetSizeInBits() == size_in_bits);
  }
};

}  // namespace internal

typedef internal::FixedSizeRegister<kXRegSize> XRegister;
typedef internal::FixedSizeRegister<kWRegSize> WRegister;


class VRegister : public CPURegister {
 public:
  VRegister() : CPURegister(), lanes_(1) {}
  explicit VRegister(const CPURegister& other)
      : CPURegister(other.GetCode(), other.GetSizeInBits(), other.GetType()),
        lanes_(1) {
    VIXL_ASSERT(IsValidVRegister());
    VIXL_ASSERT(IsPowerOf2(lanes_) && (lanes_ <= 16));
  }
  VRegister(unsigned code, unsigned size, unsigned lanes = 1)
      : CPURegister(code, size, kVRegister), lanes_(lanes) {
    VIXL_ASSERT(IsPowerOf2(lanes_) && (lanes_ <= 16));
  }
  VRegister(unsigned code, VectorFormat format)
      : CPURegister(code, RegisterSizeInBitsFromFormat(format), kVRegister),
        lanes_(IsVectorFormat(format) ? LaneCountFromFormat(format) : 1) {
    VIXL_ASSERT(IsPowerOf2(lanes_) && (lanes_ <= 16));
  }

  bool IsValid() const {
    VIXL_ASSERT(IsVRegister() || IsNone());
    return IsValidVRegister();
  }

  static const VRegister& GetBRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetBRegFromCode",
                  static const VRegister& BRegFromCode(unsigned code)) {
    return GetBRegFromCode(code);
  }

  static const VRegister& GetHRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetHRegFromCode",
                  static const VRegister& HRegFromCode(unsigned code)) {
    return GetHRegFromCode(code);
  }

  static const VRegister& GetSRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetSRegFromCode",
                  static const VRegister& SRegFromCode(unsigned code)) {
    return GetSRegFromCode(code);
  }

  static const VRegister& GetDRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetDRegFromCode",
                  static const VRegister& DRegFromCode(unsigned code)) {
    return GetDRegFromCode(code);
  }

  static const VRegister& GetQRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetQRegFromCode",
                  static const VRegister& QRegFromCode(unsigned code)) {
    return GetQRegFromCode(code);
  }

  static const VRegister& GetVRegFromCode(unsigned code);
  VIXL_DEPRECATED("GetVRegFromCode",
                  static const VRegister& VRegFromCode(unsigned code)) {
    return GetVRegFromCode(code);
  }

  VRegister V8B() const { return VRegister(code_, kDRegSize, 8); }
  VRegister V16B() const { return VRegister(code_, kQRegSize, 16); }
  VRegister V2H() const { return VRegister(code_, kSRegSize, 2); }
  VRegister V4H() const { return VRegister(code_, kDRegSize, 4); }
  VRegister V8H() const { return VRegister(code_, kQRegSize, 8); }
  VRegister V2S() const { return VRegister(code_, kDRegSize, 2); }
  VRegister V4S() const { return VRegister(code_, kQRegSize, 4); }
  VRegister V2D() const { return VRegister(code_, kQRegSize, 2); }
  VRegister V1D() const { return VRegister(code_, kDRegSize, 1); }

  bool Is8B() const { return (Is64Bits() && (lanes_ == 8)); }
  bool Is16B() const { return (Is128Bits() && (lanes_ == 16)); }
  bool Is2H() const { return (Is32Bits() && (lanes_ == 2)); }
  bool Is4H() const { return (Is64Bits() && (lanes_ == 4)); }
  bool Is8H() const { return (Is128Bits() && (lanes_ == 8)); }
  bool Is2S() const { return (Is64Bits() && (lanes_ == 2)); }
  bool Is4S() const { return (Is128Bits() && (lanes_ == 4)); }
  bool Is1D() const { return (Is64Bits() && (lanes_ == 1)); }
  bool Is2D() const { return (Is128Bits() && (lanes_ == 2)); }

  // For consistency, we assert the number of lanes of these scalar registers,
  // even though there are no vectors of equivalent total size with which they
  // could alias.
  bool Is1B() const {
    VIXL_ASSERT(!(Is8Bits() && IsVector()));
    return Is8Bits();
  }
  bool Is1H() const {
    VIXL_ASSERT(!(Is16Bits() && IsVector()));
    return Is16Bits();
  }
  bool Is1S() const {
    VIXL_ASSERT(!(Is32Bits() && IsVector()));
    return Is32Bits();
  }

  // Semantic type for sdot and udot instructions.
  bool Is1S4B() const { return Is1S(); }


  bool IsLaneSizeB() const { return GetLaneSizeInBits() == kBRegSize; }
  bool IsLaneSizeH() const { return GetLaneSizeInBits() == kHRegSize; }
  bool IsLaneSizeS() const { return GetLaneSizeInBits() == kSRegSize; }
  bool IsLaneSizeD() const { return GetLaneSizeInBits() == kDRegSize; }

  int GetLanes() const { return lanes_; }
  VIXL_DEPRECATED("GetLanes", int lanes() const) { return GetLanes(); }

  bool IsScalar() const { return lanes_ == 1; }

  bool IsVector() const { return lanes_ > 1; }

  bool IsSameFormat(const VRegister& other) const {
    return (size_ == other.size_) && (lanes_ == other.lanes_);
  }

  unsigned GetLaneSizeInBytes() const { return GetSizeInBytes() / lanes_; }
  VIXL_DEPRECATED("GetLaneSizeInBytes", unsigned LaneSizeInBytes() const) {
    return GetLaneSizeInBytes();
  }

  unsigned GetLaneSizeInBits() const { return GetLaneSizeInBytes() * 8; }
  VIXL_DEPRECATED("GetLaneSizeInBits", unsigned LaneSizeInBits() const) {
    return GetLaneSizeInBits();
  }

 private:
  static const VRegister bregisters[];
  static const VRegister hregisters[];
  static const VRegister sregisters[];
  static const VRegister dregisters[];
  static const VRegister qregisters[];
  static const VRegister vregisters[];
  int lanes_;
};


// Backward compatibility for FPRegisters.
typedef VRegister FPRegister;

// No*Reg is used to indicate an unused argument, or an error case. Note that
// these all compare equal (using the Is() method). The Register and VRegister
// variants are provided for convenience.
const Register NoReg;
const VRegister NoVReg;
const FPRegister NoFPReg;  // For backward compatibility.
const CPURegister NoCPUReg;


#define DEFINE_REGISTERS(N) \
  const WRegister w##N(N);  \
  const XRegister x##N(N);
AARCH64_REGISTER_CODE_LIST(DEFINE_REGISTERS)
#undef DEFINE_REGISTERS
const WRegister wsp(kSPRegInternalCode);
const XRegister sp(kSPRegInternalCode);


#define DEFINE_VREGISTERS(N)          \
  const VRegister b##N(N, kBRegSize); \
  const VRegister h##N(N, kHRegSize); \
  const VRegister s##N(N, kSRegSize); \
  const VRegister d##N(N, kDRegSize); \
  const VRegister q##N(N, kQRegSize); \
  const VRegister v##N(N, kQRegSize);
AARCH64_REGISTER_CODE_LIST(DEFINE_VREGISTERS)
#undef DEFINE_VREGISTERS


// Register aliases.
const XRegister ip0 = x16;
const XRegister ip1 = x17;
const XRegister lr = x30;
const XRegister xzr = x31;
const WRegister wzr = w31;


// AreAliased returns true if any of the named registers overlap. Arguments
// set to NoReg are ignored. The system stack pointer may be specified.
bool AreAliased(const CPURegister& reg1,
                const CPURegister& reg2,
                const CPURegister& reg3 = NoReg,
                const CPURegister& reg4 = NoReg,
                const CPURegister& reg5 = NoReg,
                const CPURegister& reg6 = NoReg,
                const CPURegister& reg7 = NoReg,
                const CPURegister& reg8 = NoReg);


// AreSameSizeAndType returns true if all of the specified registers have the
// same size, and are of the same type. The system stack pointer may be
// specified. Arguments set to NoReg are ignored, as are any subsequent
// arguments. At least one argument (reg1) must be valid (not NoCPUReg).
bool AreSameSizeAndType(const CPURegister& reg1,
                        const CPURegister& reg2,
                        const CPURegister& reg3 = NoCPUReg,
                        const CPURegister& reg4 = NoCPUReg,
                        const CPURegister& reg5 = NoCPUReg,
                        const CPURegister& reg6 = NoCPUReg,
                        const CPURegister& reg7 = NoCPUReg,
                        const CPURegister& reg8 = NoCPUReg);

// AreEven returns true if all of the specified registers have even register
// indices. Arguments set to NoReg are ignored, as are any subsequent
// arguments. At least one argument (reg1) must be valid (not NoCPUReg).
bool AreEven(const CPURegister& reg1,
             const CPURegister& reg2,
             const CPURegister& reg3 = NoReg,
             const CPURegister& reg4 = NoReg,
             const CPURegister& reg5 = NoReg,
             const CPURegister& reg6 = NoReg,
             const CPURegister& reg7 = NoReg,
             const CPURegister& reg8 = NoReg);


// AreConsecutive returns true if all of the specified registers are
// consecutive in the register file. Arguments set to NoReg are ignored, as are
// any subsequent arguments. At least one argument (reg1) must be valid
// (not NoCPUReg).
bool AreConsecutive(const CPURegister& reg1,
                    const CPURegister& reg2,
                    const CPURegister& reg3 = NoCPUReg,
                    const CPURegister& reg4 = NoCPUReg);


// AreSameFormat returns true if all of the specified VRegisters have the same
// vector format. Arguments set to NoReg are ignored, as are any subsequent
// arguments. At least one argument (reg1) must be valid (not NoVReg).
bool AreSameFormat(const VRegister& reg1,
                   const VRegister& reg2,
                   const VRegister& reg3 = NoVReg,
                   const VRegister& reg4 = NoVReg);


// AreConsecutive returns true if all of the specified VRegisters are
// consecutive in the register file. Arguments set to NoReg are ignored, as are
// any subsequent arguments. At least one argument (reg1) must be valid
// (not NoVReg).
bool AreConsecutive(const VRegister& reg1,
                    const VRegister& reg2,
                    const VRegister& reg3 = NoVReg,
                    const VRegister& reg4 = NoVReg);


// Lists of registers.
class CPURegList {
 public:
  explicit CPURegList(CPURegister reg1,
                      CPURegister reg2 = NoCPUReg,
                      CPURegister reg3 = NoCPUReg,
                      CPURegister reg4 = NoCPUReg)
      : list_(reg1.GetBit() | reg2.GetBit() | reg3.GetBit() | reg4.GetBit()),
        size_(reg1.GetSizeInBits()),
        type_(reg1.GetType()) {
    VIXL_ASSERT(AreSameSizeAndType(reg1, reg2, reg3, reg4));
    VIXL_ASSERT(IsValid());
  }

  CPURegList(CPURegister::RegisterType type, unsigned size, RegList list)
      : list_(list), size_(size), type_(type) {
    VIXL_ASSERT(IsValid());
  }

  CPURegList(CPURegister::RegisterType type,
             unsigned size,
             unsigned first_reg,
             unsigned last_reg)
      : size_(size), type_(type) {
    VIXL_ASSERT(
        ((type == CPURegister::kRegister) && (last_reg < kNumberOfRegisters)) ||
        ((type == CPURegister::kVRegister) &&
         (last_reg < kNumberOfVRegisters)));
    VIXL_ASSERT(last_reg >= first_reg);
    list_ = (UINT64_C(1) << (last_reg + 1)) - 1;
    list_ &= ~((UINT64_C(1) << first_reg) - 1);
    VIXL_ASSERT(IsValid());
  }

  CPURegister::RegisterType GetType() const {
    VIXL_ASSERT(IsValid());
    return type_;
  }
  VIXL_DEPRECATED("GetType", CPURegister::RegisterType type() const) {
    return GetType();
  }

  // Combine another CPURegList into this one. Registers that already exist in
  // this list are left unchanged. The type and size of the registers in the
  // 'other' list must match those in this list.
  void Combine(const CPURegList& other) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(other.GetType() == type_);
    VIXL_ASSERT(other.GetRegisterSizeInBits() == size_);
    list_ |= other.GetList();
  }

  // Remove every register in the other CPURegList from this one. Registers that
  // do not exist in this list are ignored. The type and size of the registers
  // in the 'other' list must match those in this list.
  void Remove(const CPURegList& other) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(other.GetType() == type_);
    VIXL_ASSERT(other.GetRegisterSizeInBits() == size_);
    list_ &= ~other.GetList();
  }

  // Variants of Combine and Remove which take a single register.
  void Combine(const CPURegister& other) {
    VIXL_ASSERT(other.GetType() == type_);
    VIXL_ASSERT(other.GetSizeInBits() == size_);
    Combine(other.GetCode());
  }

  void Remove(const CPURegister& other) {
    VIXL_ASSERT(other.GetType() == type_);
    VIXL_ASSERT(other.GetSizeInBits() == size_);
    Remove(other.GetCode());
  }

  // Variants of Combine and Remove which take a single register by its code;
  // the type and size of the register is inferred from this list.
  void Combine(int code) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(CPURegister(code, size_, type_).IsValid());
    list_ |= (UINT64_C(1) << code);
  }

  void Remove(int code) {
    VIXL_ASSERT(IsValid());
    VIXL_ASSERT(CPURegister(code, size_, type_).IsValid());
    list_ &= ~(UINT64_C(1) << code);
  }

  static CPURegList Union(const CPURegList& list_1, const CPURegList& list_2) {
    VIXL_ASSERT(list_1.type_ == list_2.type_);
    VIXL_ASSERT(list_1.size_ == list_2.size_);
    return CPURegList(list_1.type_, list_1.size_, list_1.list_ | list_2.list_);
  }
  static CPURegList Union(const CPURegList& list_1,
                          const CPURegList& list_2,
                          const CPURegList& list_3);
  static CPURegList Union(const CPURegList& list_1,
                          const CPURegList& list_2,
                          const CPURegList& list_3,
                          const CPURegList& list_4);

  static CPURegList Intersection(const CPURegList& list_1,
                                 const CPURegList& list_2) {
    VIXL_ASSERT(list_1.type_ == list_2.type_);
    VIXL_ASSERT(list_1.size_ == list_2.size_);
    return CPURegList(list_1.type_, list_1.size_, list_1.list_ & list_2.list_);
  }
  static CPURegList Intersection(const CPURegList& list_1,
                                 const CPURegList& list_2,
                                 const CPURegList& list_3);
  static CPURegList Intersection(const CPURegList& list_1,
                                 const CPURegList& list_2,
                                 const CPURegList& list_3,
                                 const CPURegList& list_4);

  bool Overlaps(const CPURegList& other) const {
    return (type_ == other.type_) && ((list_ & other.list_) != 0);
  }

  RegList GetList() const {
    VIXL_ASSERT(IsValid());
    return list_;
  }
  VIXL_DEPRECATED("GetList", RegList list() const) { return GetList(); }

  void SetList(RegList new_list) {
    VIXL_ASSERT(IsValid());
    list_ = new_list;
  }
  VIXL_DEPRECATED("SetList", void set_list(RegList new_list)) {
    return SetList(new_list);
  }

  // Remove all callee-saved registers from the list. This can be useful when
  // preparing registers for an AAPCS64 function call, for example.
  void RemoveCalleeSaved();

  CPURegister PopLowestIndex();
  CPURegister PopHighestIndex();

  // AAPCS64 callee-saved registers.
  static CPURegList GetCalleeSaved(unsigned size = kXRegSize);
  static CPURegList GetCalleeSavedV(unsigned size = kDRegSize);

  // AAPCS64 caller-saved registers. Note that this includes lr.
  // TODO(all): Determine how we handle d8-d15 being callee-saved, but the top
  // 64-bits being caller-saved.
  static CPURegList GetCallerSaved(unsigned size = kXRegSize);
  static CPURegList GetCallerSavedV(unsigned size = kDRegSize);

  bool IsEmpty() const {
    VIXL_ASSERT(IsValid());
    return list_ == 0;
  }

  bool IncludesAliasOf(const CPURegister& other) const {
    VIXL_ASSERT(IsValid());
    return (type_ == other.GetType()) && ((other.GetBit() & list_) != 0);
  }

  bool IncludesAliasOf(int code) const {
    VIXL_ASSERT(IsValid());
    return ((code & list_) != 0);
  }

  int GetCount() const {
    VIXL_ASSERT(IsValid());
    return CountSetBits(list_);
  }
  VIXL_DEPRECATED("GetCount", int Count()) const { return GetCount(); }

  int GetRegisterSizeInBits() const {
    VIXL_ASSERT(IsValid());
    return size_;
  }
  VIXL_DEPRECATED("GetRegisterSizeInBits", int RegisterSizeInBits() const) {
    return GetRegisterSizeInBits();
  }

  int GetRegisterSizeInBytes() const {
    int size_in_bits = GetRegisterSizeInBits();
    VIXL_ASSERT((size_in_bits % 8) == 0);
    return size_in_bits / 8;
  }
  VIXL_DEPRECATED("GetRegisterSizeInBytes", int RegisterSizeInBytes() const) {
    return GetRegisterSizeInBytes();
  }

  unsigned GetTotalSizeInBytes() const {
    VIXL_ASSERT(IsValid());
    return GetRegisterSizeInBytes() * GetCount();
  }
  VIXL_DEPRECATED("GetTotalSizeInBytes", unsigned TotalSizeInBytes() const) {
    return GetTotalSizeInBytes();
  }

 private:
  RegList list_;
  int size_;
  CPURegister::RegisterType type_;

  bool IsValid() const;
};


// AAPCS64 callee-saved registers.
extern const CPURegList kCalleeSaved;
extern const CPURegList kCalleeSavedV;


// AAPCS64 caller-saved registers. Note that this includes lr.
extern const CPURegList kCallerSaved;
extern const CPURegList kCallerSavedV;


// Operand.
class Operand {
 public:
  // #<immediate>
  // where <immediate> is int64_t.
  // This is allowed to be an implicit constructor because Operand is
  // a wrapper class that doesn't normally perform any type conversion.
  Operand(int64_t immediate = 0);  // NOLINT(runtime/explicit)

  // rm, {<shift> #<shift_amount>}
  // where <shift> is one of {LSL, LSR, ASR, ROR}.
  //       <shift_amount> is uint6_t.
  // This is allowed to be an implicit constructor because Operand is
  // a wrapper class that doesn't normally perform any type conversion.
  Operand(Register reg,
          Shift shift = LSL,
          unsigned shift_amount = 0);  // NOLINT(runtime/explicit)

  // rm, {<extend> {#<shift_amount>}}
  // where <extend> is one of {UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX}.
  //       <shift_amount> is uint2_t.
  explicit Operand(Register reg, Extend extend, unsigned shift_amount = 0);

  bool IsImmediate() const;
  bool IsPlainRegister() const;
  bool IsShiftedRegister() const;
  bool IsExtendedRegister() const;
  bool IsZero() const;

  // This returns an LSL shift (<= 4) operand as an equivalent extend operand,
  // which helps in the encoding of instructions that use the stack pointer.
  Operand ToExtendedRegister() const;

  int64_t GetImmediate() const {
    VIXL_ASSERT(IsImmediate());
    return immediate_;
  }
  VIXL_DEPRECATED("GetImmediate", int64_t immediate() const) {
    return GetImmediate();
  }

  int64_t GetEquivalentImmediate() const {
    return IsZero() ? 0 : GetImmediate();
  }

  Register GetRegister() const {
    VIXL_ASSERT(IsShiftedRegister() || IsExtendedRegister());
    return reg_;
  }
  VIXL_DEPRECATED("GetRegister", Register reg() const) { return GetRegister(); }
  Register GetBaseRegister() const { return GetRegister(); }

  Shift GetShift() const {
    VIXL_ASSERT(IsShiftedRegister());
    return shift_;
  }
  VIXL_DEPRECATED("GetShift", Shift shift() const) { return GetShift(); }

  Extend GetExtend() const {
    VIXL_ASSERT(IsExtendedRegister());
    return extend_;
  }
  VIXL_DEPRECATED("GetExtend", Extend extend() const) { return GetExtend(); }

  unsigned GetShiftAmount() const {
    VIXL_ASSERT(IsShiftedRegister() || IsExtendedRegister());
    return shift_amount_;
  }
  VIXL_DEPRECATED("GetShiftAmount", unsigned shift_amount() const) {
    return GetShiftAmount();
  }

 private:
  int64_t immediate_;
  Register reg_;
  Shift shift_;
  Extend extend_;
  unsigned shift_amount_;
};


// MemOperand represents the addressing mode of a load or store instruction.
class MemOperand {
 public:
  // Creates an invalid `MemOperand`.
  MemOperand();
  explicit MemOperand(Register base,
                      int64_t offset = 0,
                      AddrMode addrmode = Offset);
  MemOperand(Register base,
             Register regoffset,
             Shift shift = LSL,
             unsigned shift_amount = 0);
  MemOperand(Register base,
             Register regoffset,
             Extend extend,
             unsigned shift_amount = 0);
  MemOperand(Register base, const Operand& offset, AddrMode addrmode = Offset);

  const Register& GetBaseRegister() const { return base_; }
  VIXL_DEPRECATED("GetBaseRegister", const Register& base() const) {
    return GetBaseRegister();
  }

  const Register& GetRegisterOffset() const { return regoffset_; }
  VIXL_DEPRECATED("GetRegisterOffset", const Register& regoffset() const) {
    return GetRegisterOffset();
  }

  int64_t GetOffset() const { return offset_; }
  VIXL_DEPRECATED("GetOffset", int64_t offset() const) { return GetOffset(); }

  AddrMode GetAddrMode() const { return addrmode_; }
  VIXL_DEPRECATED("GetAddrMode", AddrMode addrmode() const) {
    return GetAddrMode();
  }

  Shift GetShift() const { return shift_; }
  VIXL_DEPRECATED("GetShift", Shift shift() const) { return GetShift(); }

  Extend GetExtend() const { return extend_; }
  VIXL_DEPRECATED("GetExtend", Extend extend() const) { return GetExtend(); }

  unsigned GetShiftAmount() const { return shift_amount_; }
  VIXL_DEPRECATED("GetShiftAmount", unsigned shift_amount() const) {
    return GetShiftAmount();
  }

  bool IsImmediateOffset() const;
  bool IsRegisterOffset() const;
  bool IsPreIndex() const;
  bool IsPostIndex() const;

  void AddOffset(int64_t offset);

  bool IsValid() const {
    return base_.IsValid() &&
           ((addrmode_ == Offset) || (addrmode_ == PreIndex) ||
            (addrmode_ == PostIndex)) &&
           ((shift_ == NO_SHIFT) || (extend_ == NO_EXTEND)) &&
           ((offset_ == 0) || !regoffset_.IsValid());
  }

  bool Equals(const MemOperand& other) const {
    return base_.Is(other.base_) && regoffset_.Is(other.regoffset_) &&
           (offset_ == other.offset_) && (addrmode_ == other.addrmode_) &&
           (shift_ == other.shift_) && (extend_ == other.extend_) &&
           (shift_amount_ == other.shift_amount_);
  }

 private:
  Register base_;
  Register regoffset_;
  int64_t offset_;
  AddrMode addrmode_;
  Shift shift_;
  Extend extend_;
  unsigned shift_amount_;
};

// This an abstraction that can represent a register or memory location. The
// `MacroAssembler` provides helpers to move data between generic operands.
class GenericOperand {
 public:
  GenericOperand() { VIXL_ASSERT(!IsValid()); }
  GenericOperand(const CPURegister& reg);  // NOLINT(runtime/explicit)
  GenericOperand(const MemOperand& mem_op,
                 size_t mem_op_size = 0);  // NOLINT(runtime/explicit)

  bool IsValid() const { return cpu_register_.IsValid() != mem_op_.IsValid(); }

  bool Equals(const GenericOperand& other) const;

  bool IsCPURegister() const {
    VIXL_ASSERT(IsValid());
    return cpu_register_.IsValid();
  }

  bool IsRegister() const {
    return IsCPURegister() && cpu_register_.IsRegister();
  }

  bool IsVRegister() const {
    return IsCPURegister() && cpu_register_.IsVRegister();
  }

  bool IsSameCPURegisterType(const GenericOperand& other) {
    return IsCPURegister() && other.IsCPURegister() &&
           GetCPURegister().IsSameType(other.GetCPURegister());
  }

  bool IsMemOperand() const {
    VIXL_ASSERT(IsValid());
    return mem_op_.IsValid();
  }

  CPURegister GetCPURegister() const {
    VIXL_ASSERT(IsCPURegister());
    return cpu_register_;
  }

  MemOperand GetMemOperand() const {
    VIXL_ASSERT(IsMemOperand());
    return mem_op_;
  }

  size_t GetMemOperandSizeInBytes() const {
    VIXL_ASSERT(IsMemOperand());
    return mem_op_size_;
  }

  size_t GetSizeInBytes() const {
    return IsCPURegister() ? cpu_register_.GetSizeInBytes()
                           : GetMemOperandSizeInBytes();
  }

  size_t GetSizeInBits() const { return GetSizeInBytes() * kBitsPerByte; }

 private:
  CPURegister cpu_register_;
  MemOperand mem_op_;
  // The size of the memory region pointed to, in bytes.
  // We only support sizes up to X/D register sizes.
  size_t mem_op_size_;
};
}
}  // namespace vixl::aarch64

#endif  // VIXL_AARCH64_OPERANDS_AARCH64_H_
