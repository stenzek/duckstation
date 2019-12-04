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

#ifndef VIXL_AARCH32_INSTRUCTIONS_AARCH32_H_
#define VIXL_AARCH32_INSTRUCTIONS_AARCH32_H_

extern "C" {
#include <stdint.h>
}

#include <algorithm>
#include <ostream>

#include "code-buffer-vixl.h"
#include "utils-vixl.h"
#include "aarch32/constants-aarch32.h"

#ifdef __arm__
#define HARDFLOAT __attribute__((noinline, pcs("aapcs-vfp")))
#else
#define HARDFLOAT __attribute__((noinline))
#endif

namespace vixl {
namespace aarch32 {

class Operand;
class SOperand;
class DOperand;
class QOperand;
class MemOperand;
class AlignedMemOperand;

enum AddrMode { Offset = 0, PreIndex = 1, PostIndex = 2 };

class CPURegister {
 public:
  enum RegisterType {
    kNoRegister = 0,
    kRRegister = 1,
    kSRegister = 2,
    kDRegister = 3,
    kQRegister = 4
  };

 private:
  static const int kCodeBits = 5;
  static const int kTypeBits = 4;
  static const int kSizeBits = 8;
  static const int kCodeShift = 0;
  static const int kTypeShift = kCodeShift + kCodeBits;
  static const int kSizeShift = kTypeShift + kTypeBits;
  static const uint32_t kCodeMask = ((1 << kCodeBits) - 1) << kCodeShift;
  static const uint32_t kTypeMask = ((1 << kTypeBits) - 1) << kTypeShift;
  static const uint32_t kSizeMask = ((1 << kSizeBits) - 1) << kSizeShift;
  uint32_t value_;

 public:
  CPURegister(RegisterType type, uint32_t code, int size)
      : value_((type << kTypeShift) | (code << kCodeShift) |
               (size << kSizeShift)) {
#ifdef VIXL_DEBUG
    switch (type) {
      case kNoRegister:
        break;
      case kRRegister:
        VIXL_ASSERT(code < kNumberOfRegisters);
        VIXL_ASSERT(size == kRegSizeInBits);
        break;
      case kSRegister:
        VIXL_ASSERT(code < kNumberOfSRegisters);
        VIXL_ASSERT(size == kSRegSizeInBits);
        break;
      case kDRegister:
        VIXL_ASSERT(code < kMaxNumberOfDRegisters);
        VIXL_ASSERT(size == kDRegSizeInBits);
        break;
      case kQRegister:
        VIXL_ASSERT(code < kNumberOfQRegisters);
        VIXL_ASSERT(size == kQRegSizeInBits);
        break;
      default:
        VIXL_UNREACHABLE();
        break;
    }
#endif
  }
  RegisterType GetType() const {
    return static_cast<RegisterType>((value_ & kTypeMask) >> kTypeShift);
  }
  bool IsRegister() const { return GetType() == kRRegister; }
  bool IsS() const { return GetType() == kSRegister; }
  bool IsD() const { return GetType() == kDRegister; }
  bool IsQ() const { return GetType() == kQRegister; }
  bool IsVRegister() const { return IsS() || IsD() || IsQ(); }
  bool IsFPRegister() const { return IsS() || IsD(); }
  uint32_t GetCode() const { return (value_ & kCodeMask) >> kCodeShift; }
  uint32_t GetReg() const { return value_; }
  int GetSizeInBits() const { return (value_ & kSizeMask) >> kSizeShift; }
  int GetRegSizeInBytes() const {
    return (GetType() == kNoRegister) ? 0 : (GetSizeInBits() / 8);
  }
  bool Is64Bits() const { return GetSizeInBits() == 64; }
  bool Is128Bits() const { return GetSizeInBits() == 128; }
  bool IsSameFormat(CPURegister reg) {
    return (value_ & ~kCodeMask) == (reg.value_ & ~kCodeMask);
  }
  bool Is(CPURegister ref) const { return GetReg() == ref.GetReg(); }
  bool IsValid() const { return GetType() != kNoRegister; }
};

class Register : public CPURegister {
 public:
  Register() : CPURegister(kNoRegister, 0, kRegSizeInBits) {}
  explicit Register(uint32_t code)
      : CPURegister(kRRegister, code % kNumberOfRegisters, kRegSizeInBits) {
    VIXL_ASSERT(GetCode() < kNumberOfRegisters);
  }
  bool Is(Register ref) const { return GetCode() == ref.GetCode(); }
  bool IsLow() const { return GetCode() < kNumberOfT32LowRegisters; }
  bool IsLR() const { return GetCode() == kLrCode; }
  bool IsPC() const { return GetCode() == kPcCode; }
  bool IsSP() const { return GetCode() == kSpCode; }
};

std::ostream& operator<<(std::ostream& os, const Register reg);

class RegisterOrAPSR_nzcv {
  uint32_t code_;

 public:
  explicit RegisterOrAPSR_nzcv(uint32_t code) : code_(code) {
    VIXL_ASSERT(code_ < kNumberOfRegisters);
  }
  bool IsAPSR_nzcv() const { return code_ == kPcCode; }
  uint32_t GetCode() const { return code_; }
  Register AsRegister() const {
    VIXL_ASSERT(!IsAPSR_nzcv());
    return Register(code_);
  }
};

const RegisterOrAPSR_nzcv APSR_nzcv(kPcCode);

inline std::ostream& operator<<(std::ostream& os,
                                const RegisterOrAPSR_nzcv reg) {
  if (reg.IsAPSR_nzcv()) return os << "APSR_nzcv";
  return os << reg.AsRegister();
}

class SRegister;
class DRegister;
class QRegister;

class VRegister : public CPURegister {
 public:
  VRegister() : CPURegister(kNoRegister, 0, 0) {}
  VRegister(RegisterType type, uint32_t code, int size)
      : CPURegister(type, code, size) {}

  SRegister S() const;
  DRegister D() const;
  QRegister Q() const;
};

class SRegister : public VRegister {
 public:
  SRegister() : VRegister(kNoRegister, 0, kSRegSizeInBits) {}
  explicit SRegister(uint32_t code)
      : VRegister(kSRegister, code, kSRegSizeInBits) {}
  uint32_t Encode(int single_bit_field, int four_bit_field_lowest_bit) const {
    if (four_bit_field_lowest_bit == 0) {
      return ((GetCode() & 0x1) << single_bit_field) |
             ((GetCode() & 0x1e) >> 1);
    }
    return ((GetCode() & 0x1) << single_bit_field) |
           ((GetCode() & 0x1e) << (four_bit_field_lowest_bit - 1));
  }
};

inline unsigned ExtractSRegister(uint32_t instr,
                                 int single_bit_field,
                                 int four_bit_field_lowest_bit) {
  VIXL_ASSERT(single_bit_field > 0);
  if (four_bit_field_lowest_bit == 0) {
    return ((instr << 1) & 0x1e) | ((instr >> single_bit_field) & 0x1);
  }
  return ((instr >> (four_bit_field_lowest_bit - 1)) & 0x1e) |
         ((instr >> single_bit_field) & 0x1);
}

inline std::ostream& operator<<(std::ostream& os, const SRegister reg) {
  return os << "s" << reg.GetCode();
}

class DRegister : public VRegister {
 public:
  DRegister() : VRegister(kNoRegister, 0, kDRegSizeInBits) {}
  explicit DRegister(uint32_t code)
      : VRegister(kDRegister, code, kDRegSizeInBits) {}
  SRegister GetLane(uint32_t lane) const {
    uint32_t lane_count = kDRegSizeInBits / kSRegSizeInBits;
    VIXL_ASSERT(lane < lane_count);
    VIXL_ASSERT(GetCode() * lane_count < kNumberOfSRegisters);
    return SRegister(GetCode() * lane_count + lane);
  }
  uint32_t Encode(int single_bit_field, int four_bit_field_lowest_bit) const {
    VIXL_ASSERT(single_bit_field >= 4);
    return ((GetCode() & 0x10) << (single_bit_field - 4)) |
           ((GetCode() & 0xf) << four_bit_field_lowest_bit);
  }
};

inline unsigned ExtractDRegister(uint32_t instr,
                                 int single_bit_field,
                                 int four_bit_field_lowest_bit) {
  VIXL_ASSERT(single_bit_field >= 4);
  return ((instr >> (single_bit_field - 4)) & 0x10) |
         ((instr >> four_bit_field_lowest_bit) & 0xf);
}

inline std::ostream& operator<<(std::ostream& os, const DRegister reg) {
  return os << "d" << reg.GetCode();
}

enum DataTypeType {
  kDataTypeS = 0x100,
  kDataTypeU = 0x200,
  kDataTypeF = 0x300,
  kDataTypeI = 0x400,
  kDataTypeP = 0x500,
  kDataTypeUntyped = 0x600
};
const int kDataTypeSizeMask = 0x0ff;
const int kDataTypeTypeMask = 0x100;
enum DataTypeValue {
  kDataTypeValueInvalid = 0x000,
  kDataTypeValueNone = 0x001,  // value used when dt is ignored.
  S8 = kDataTypeS | 8,
  S16 = kDataTypeS | 16,
  S32 = kDataTypeS | 32,
  S64 = kDataTypeS | 64,
  U8 = kDataTypeU | 8,
  U16 = kDataTypeU | 16,
  U32 = kDataTypeU | 32,
  U64 = kDataTypeU | 64,
  F16 = kDataTypeF | 16,
  F32 = kDataTypeF | 32,
  F64 = kDataTypeF | 64,
  I8 = kDataTypeI | 8,
  I16 = kDataTypeI | 16,
  I32 = kDataTypeI | 32,
  I64 = kDataTypeI | 64,
  P8 = kDataTypeP | 8,
  P64 = kDataTypeP | 64,
  Untyped8 = kDataTypeUntyped | 8,
  Untyped16 = kDataTypeUntyped | 16,
  Untyped32 = kDataTypeUntyped | 32,
  Untyped64 = kDataTypeUntyped | 64
};

class DataType {
  DataTypeValue value_;

 public:
  explicit DataType(uint32_t size)
      : value_(static_cast<DataTypeValue>(kDataTypeUntyped | size)) {
    VIXL_ASSERT((size == 8) || (size == 16) || (size == 32) || (size == 64));
  }
  // Users should be able to use "S8", "S6" and so forth to instantiate this
  // class.
  DataType(DataTypeValue value) : value_(value) {}  // NOLINT(runtime/explicit)
  DataTypeValue GetValue() const { return value_; }
  DataTypeType GetType() const {
    return static_cast<DataTypeType>(value_ & kDataTypeTypeMask);
  }
  uint32_t GetSize() const { return value_ & kDataTypeSizeMask; }
  bool IsSize(uint32_t size) const {
    return (value_ & kDataTypeSizeMask) == size;
  }
  const char* GetName() const;
  bool Is(DataType type) const { return value_ == type.value_; }
  bool Is(DataTypeValue value) const { return value_ == value; }
  bool Is(DataTypeType type) const { return GetType() == type; }
  bool IsNoneOr(DataTypeValue value) const {
    return (value_ == value) || (value_ == kDataTypeValueNone);
  }
  bool Is(DataTypeType type, uint32_t size) const {
    return value_ == static_cast<DataTypeValue>(type | size);
  }
  bool IsNoneOr(DataTypeType type, uint32_t size) const {
    return Is(type, size) || Is(kDataTypeValueNone);
  }
};

inline std::ostream& operator<<(std::ostream& os, DataType dt) {
  return os << dt.GetName();
}

class DRegisterLane : public DRegister {
  uint32_t lane_;

 public:
  DRegisterLane(DRegister reg, uint32_t lane)
      : DRegister(reg.GetCode()), lane_(lane) {}
  DRegisterLane(uint32_t code, uint32_t lane) : DRegister(code), lane_(lane) {}
  uint32_t GetLane() const { return lane_; }
  uint32_t EncodeX(DataType dt,
                   int single_bit_field,
                   int four_bit_field_lowest_bit) const {
    VIXL_ASSERT(single_bit_field >= 4);
    uint32_t value = lane_ << ((dt.GetSize() == 16) ? 3 : 4) | GetCode();
    return ((value & 0x10) << (single_bit_field - 4)) |
           ((value & 0xf) << four_bit_field_lowest_bit);
  }
};

inline unsigned ExtractDRegisterAndLane(uint32_t instr,
                                        DataType dt,
                                        int single_bit_field,
                                        int four_bit_field_lowest_bit,
                                        int* lane) {
  VIXL_ASSERT(single_bit_field >= 4);
  uint32_t value = ((instr >> (single_bit_field - 4)) & 0x10) |
                   ((instr >> four_bit_field_lowest_bit) & 0xf);
  if (dt.GetSize() == 16) {
    *lane = value >> 3;
    return value & 0x7;
  }
  *lane = value >> 4;
  return value & 0xf;
}

inline std::ostream& operator<<(std::ostream& os, const DRegisterLane lane) {
  os << "d" << lane.GetCode() << "[";
  if (lane.GetLane() == static_cast<uint32_t>(-1)) return os << "??]";
  return os << lane.GetLane() << "]";
}

class QRegister : public VRegister {
 public:
  QRegister() : VRegister(kNoRegister, 0, kQRegSizeInBits) {}
  explicit QRegister(uint32_t code)
      : VRegister(kQRegister, code, kQRegSizeInBits) {}
  uint32_t Encode(int offset) { return GetCode() << offset; }
  DRegister GetDLane(uint32_t lane) const {
    uint32_t lane_count = kQRegSizeInBits / kDRegSizeInBits;
    VIXL_ASSERT(lane < lane_count);
    return DRegister(GetCode() * lane_count + lane);
  }
  DRegister GetLowDRegister() const { return DRegister(GetCode() * 2); }
  DRegister GetHighDRegister() const { return DRegister(1 + GetCode() * 2); }
  SRegister GetSLane(uint32_t lane) const {
    uint32_t lane_count = kQRegSizeInBits / kSRegSizeInBits;
    VIXL_ASSERT(lane < lane_count);
    VIXL_ASSERT(GetCode() * lane_count < kNumberOfSRegisters);
    return SRegister(GetCode() * lane_count + lane);
  }
  uint32_t Encode(int single_bit_field, int four_bit_field_lowest_bit) {
    // Encode "code * 2".
    VIXL_ASSERT(single_bit_field >= 3);
    return ((GetCode() & 0x8) << (single_bit_field - 3)) |
           ((GetCode() & 0x7) << (four_bit_field_lowest_bit + 1));
  }
};

inline unsigned ExtractQRegister(uint32_t instr,
                                 int single_bit_field,
                                 int four_bit_field_lowest_bit) {
  VIXL_ASSERT(single_bit_field >= 3);
  return ((instr >> (single_bit_field - 3)) & 0x8) |
         ((instr >> (four_bit_field_lowest_bit + 1)) & 0x7);
}

inline std::ostream& operator<<(std::ostream& os, const QRegister reg) {
  return os << "q" << reg.GetCode();
}

// clang-format off
#define AARCH32_REGISTER_CODE_LIST(R)                                          \
  R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7)                               \
  R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15)
// clang-format on
#define DEFINE_REGISTER(N) const Register r##N(N);
AARCH32_REGISTER_CODE_LIST(DEFINE_REGISTER)
#undef DEFINE_REGISTER
#undef AARCH32_REGISTER_CODE_LIST

enum RegNum { kIPRegNum = 12, kSPRegNum = 13, kLRRegNum = 14, kPCRegNum = 15 };

const Register ip(kIPRegNum);
const Register sp(kSPRegNum);
const Register pc(kPCRegNum);
const Register lr(kLRRegNum);
const Register NoReg;
const VRegister NoVReg;

// clang-format off
#define SREGISTER_CODE_LIST(R)                                                 \
  R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7)                               \
  R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15)                              \
  R(16) R(17) R(18) R(19) R(20) R(21) R(22) R(23)                              \
  R(24) R(25) R(26) R(27) R(28) R(29) R(30) R(31)
// clang-format on
#define DEFINE_REGISTER(N) const SRegister s##N(N);
SREGISTER_CODE_LIST(DEFINE_REGISTER)
#undef DEFINE_REGISTER
#undef SREGISTER_CODE_LIST
const SRegister NoSReg;

// clang-format off
#define DREGISTER_CODE_LIST(R)                                                 \
R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7)                                 \
R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15)                                \
R(16) R(17) R(18) R(19) R(20) R(21) R(22) R(23)                                \
R(24) R(25) R(26) R(27) R(28) R(29) R(30) R(31)
// clang-format on
#define DEFINE_REGISTER(N) const DRegister d##N(N);
DREGISTER_CODE_LIST(DEFINE_REGISTER)
#undef DEFINE_REGISTER
#undef DREGISTER_CODE_LIST
const DRegister NoDReg;

// clang-format off
#define QREGISTER_CODE_LIST(R)                                                 \
  R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7)                               \
  R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15)
// clang-format on
#define DEFINE_REGISTER(N) const QRegister q##N(N);
QREGISTER_CODE_LIST(DEFINE_REGISTER)
#undef DEFINE_REGISTER
#undef QREGISTER_CODE_LIST
const QRegister NoQReg;

class RegisterList {
 public:
  RegisterList() : list_(0) {}
  RegisterList(Register reg)  // NOLINT(runtime/explicit)
      : list_(RegisterToList(reg)) {}
  RegisterList(Register reg1, Register reg2)
      : list_(RegisterToList(reg1) | RegisterToList(reg2)) {}
  RegisterList(Register reg1, Register reg2, Register reg3)
      : list_(RegisterToList(reg1) | RegisterToList(reg2) |
              RegisterToList(reg3)) {}
  RegisterList(Register reg1, Register reg2, Register reg3, Register reg4)
      : list_(RegisterToList(reg1) | RegisterToList(reg2) |
              RegisterToList(reg3) | RegisterToList(reg4)) {}
  explicit RegisterList(uint32_t list) : list_(list) {}
  uint32_t GetList() const { return list_; }
  void SetList(uint32_t list) { list_ = list; }
  bool Includes(const Register& reg) const {
    return (list_ & RegisterToList(reg)) != 0;
  }
  void Combine(const RegisterList& other) { list_ |= other.GetList(); }
  void Combine(const Register& reg) { list_ |= RegisterToList(reg); }
  void Remove(const RegisterList& other) { list_ &= ~other.GetList(); }
  void Remove(const Register& reg) { list_ &= ~RegisterToList(reg); }
  bool Overlaps(const RegisterList& other) const {
    return (list_ & other.list_) != 0;
  }
  bool IsR0toR7orPC() const {
    // True if all the registers from the list are not from r8-r14.
    return (list_ & 0x7f00) == 0;
  }
  bool IsR0toR7orLR() const {
    // True if all the registers from the list are not from r8-r13 nor from r15.
    return (list_ & 0xbf00) == 0;
  }
  Register GetFirstAvailableRegister() const;
  bool IsEmpty() const { return list_ == 0; }
  static RegisterList Union(const RegisterList& list_1,
                            const RegisterList& list_2) {
    return RegisterList(list_1.list_ | list_2.list_);
  }
  static RegisterList Union(const RegisterList& list_1,
                            const RegisterList& list_2,
                            const RegisterList& list_3) {
    return Union(list_1, Union(list_2, list_3));
  }
  static RegisterList Union(const RegisterList& list_1,
                            const RegisterList& list_2,
                            const RegisterList& list_3,
                            const RegisterList& list_4) {
    return Union(Union(list_1, list_2), Union(list_3, list_4));
  }
  static RegisterList Intersection(const RegisterList& list_1,
                                   const RegisterList& list_2) {
    return RegisterList(list_1.list_ & list_2.list_);
  }
  static RegisterList Intersection(const RegisterList& list_1,
                                   const RegisterList& list_2,
                                   const RegisterList& list_3) {
    return Intersection(list_1, Intersection(list_2, list_3));
  }
  static RegisterList Intersection(const RegisterList& list_1,
                                   const RegisterList& list_2,
                                   const RegisterList& list_3,
                                   const RegisterList& list_4) {
    return Intersection(Intersection(list_1, list_2),
                        Intersection(list_3, list_4));
  }

 private:
  static uint32_t RegisterToList(Register reg) {
    if (reg.GetType() == CPURegister::kNoRegister) {
      return 0;
    } else {
      return UINT32_C(1) << reg.GetCode();
    }
  }

  // Bitfield representation of all registers in the list
  // (1 for r0, 2 for r1, 4 for r2, ...).
  uint32_t list_;
};

inline uint32_t GetRegisterListEncoding(const RegisterList& registers,
                                        int first,
                                        int count) {
  return (registers.GetList() >> first) & ((1 << count) - 1);
}

std::ostream& operator<<(std::ostream& os, RegisterList registers);

class VRegisterList {
 public:
  VRegisterList() : list_(0) {}
  explicit VRegisterList(VRegister reg) : list_(RegisterToList(reg)) {}
  VRegisterList(VRegister reg1, VRegister reg2)
      : list_(RegisterToList(reg1) | RegisterToList(reg2)) {}
  VRegisterList(VRegister reg1, VRegister reg2, VRegister reg3)
      : list_(RegisterToList(reg1) | RegisterToList(reg2) |
              RegisterToList(reg3)) {}
  VRegisterList(VRegister reg1, VRegister reg2, VRegister reg3, VRegister reg4)
      : list_(RegisterToList(reg1) | RegisterToList(reg2) |
              RegisterToList(reg3) | RegisterToList(reg4)) {}
  explicit VRegisterList(uint64_t list) : list_(list) {}
  uint64_t GetList() const { return list_; }
  void SetList(uint64_t list) { list_ = list; }
  // Because differently-sized V registers overlap with one another, there is no
  // way to implement a single 'Includes' function in a way that is unsurprising
  // for all existing uses.
  bool IncludesAllOf(const VRegister& reg) const {
    return (list_ & RegisterToList(reg)) == RegisterToList(reg);
  }
  bool IncludesAliasOf(const VRegister& reg) const {
    return (list_ & RegisterToList(reg)) != 0;
  }
  void Combine(const VRegisterList& other) { list_ |= other.GetList(); }
  void Combine(const VRegister& reg) { list_ |= RegisterToList(reg); }
  void Remove(const VRegisterList& other) { list_ &= ~other.GetList(); }
  void Remove(const VRegister& reg) { list_ &= ~RegisterToList(reg); }
  bool Overlaps(const VRegisterList& other) const {
    return (list_ & other.list_) != 0;
  }
  QRegister GetFirstAvailableQRegister() const;
  DRegister GetFirstAvailableDRegister() const;
  SRegister GetFirstAvailableSRegister() const;
  bool IsEmpty() const { return list_ == 0; }
  static VRegisterList Union(const VRegisterList& list_1,
                             const VRegisterList& list_2) {
    return VRegisterList(list_1.list_ | list_2.list_);
  }
  static VRegisterList Union(const VRegisterList& list_1,
                             const VRegisterList& list_2,
                             const VRegisterList& list_3) {
    return Union(list_1, Union(list_2, list_3));
  }
  static VRegisterList Union(const VRegisterList& list_1,
                             const VRegisterList& list_2,
                             const VRegisterList& list_3,
                             const VRegisterList& list_4) {
    return Union(Union(list_1, list_2), Union(list_3, list_4));
  }
  static VRegisterList Intersection(const VRegisterList& list_1,
                                    const VRegisterList& list_2) {
    return VRegisterList(list_1.list_ & list_2.list_);
  }
  static VRegisterList Intersection(const VRegisterList& list_1,
                                    const VRegisterList& list_2,
                                    const VRegisterList& list_3) {
    return Intersection(list_1, Intersection(list_2, list_3));
  }
  static VRegisterList Intersection(const VRegisterList& list_1,
                                    const VRegisterList& list_2,
                                    const VRegisterList& list_3,
                                    const VRegisterList& list_4) {
    return Intersection(Intersection(list_1, list_2),
                        Intersection(list_3, list_4));
  }

 private:
  static uint64_t RegisterToList(VRegister reg) {
    if (reg.GetType() == CPURegister::kNoRegister) {
      return 0;
    } else {
      switch (reg.GetSizeInBits()) {
        case kQRegSizeInBits:
          return UINT64_C(0xf) << (reg.GetCode() * 4);
        case kDRegSizeInBits:
          return UINT64_C(0x3) << (reg.GetCode() * 2);
        case kSRegSizeInBits:
          return UINT64_C(0x1) << reg.GetCode();
        default:
          VIXL_UNREACHABLE();
          return 0;
      }
    }
  }

  // Bitfield representation of all registers in the list.
  // (0x3 for d0, 0xc0 for d1, 0x30 for d2, ...). We have one, two or four bits
  // per register according to their size. This way we can make sure that we
  // account for overlapping registers.
  // A register is wholly included in this list only if all of its bits are set.
  // A register is aliased by the list if at least one of its bits are set.
  // The IncludesAllOf and IncludesAliasOf helpers are provided to make this
  // distinction clear.
  uint64_t list_;
};

class SRegisterList {
  SRegister first_;
  int length_;

 public:
  explicit SRegisterList(SRegister reg) : first_(reg.GetCode()), length_(1) {}
  SRegisterList(SRegister first, int length)
      : first_(first.GetCode()), length_(length) {
    VIXL_ASSERT(length >= 0);
  }
  SRegister GetSRegister(int n) const {
    VIXL_ASSERT(n >= 0);
    VIXL_ASSERT(n < length_);
    return SRegister((first_.GetCode() + n) % kNumberOfSRegisters);
  }
  const SRegister& GetFirstSRegister() const { return first_; }
  SRegister GetLastSRegister() const { return GetSRegister(length_ - 1); }
  int GetLength() const { return length_; }
};

std::ostream& operator<<(std::ostream& os, SRegisterList registers);

class DRegisterList {
  DRegister first_;
  int length_;

 public:
  explicit DRegisterList(DRegister reg) : first_(reg.GetCode()), length_(1) {}
  DRegisterList(DRegister first, int length)
      : first_(first.GetCode()), length_(length) {
    VIXL_ASSERT(length >= 0);
  }
  DRegister GetDRegister(int n) const {
    VIXL_ASSERT(n >= 0);
    VIXL_ASSERT(n < length_);
    return DRegister((first_.GetCode() + n) % kMaxNumberOfDRegisters);
  }
  const DRegister& GetFirstDRegister() const { return first_; }
  DRegister GetLastDRegister() const { return GetDRegister(length_ - 1); }
  int GetLength() const { return length_; }
};

std::ostream& operator<<(std::ostream& os, DRegisterList registers);

enum SpacingType { kSingle, kDouble };

enum TransferType { kMultipleLanes, kOneLane, kAllLanes };

class NeonRegisterList {
  DRegister first_;
  SpacingType spacing_;
  TransferType type_;
  int lane_;
  int length_;

 public:
  NeonRegisterList(DRegister reg, TransferType type)
      : first_(reg.GetCode()),
        spacing_(kSingle),
        type_(type),
        lane_(-1),
        length_(1) {
    VIXL_ASSERT(type_ != kOneLane);
  }
  NeonRegisterList(DRegister reg, int lane)
      : first_(reg.GetCode()),
        spacing_(kSingle),
        type_(kOneLane),
        lane_(lane),
        length_(1) {
    VIXL_ASSERT((lane_ >= 0) && (lane_ < 8));
  }
  NeonRegisterList(DRegister first,
                   DRegister last,
                   SpacingType spacing,
                   TransferType type)
      : first_(first.GetCode()), spacing_(spacing), type_(type), lane_(-1) {
    VIXL_ASSERT(type != kOneLane);
    VIXL_ASSERT(first.GetCode() <= last.GetCode());

    int range = last.GetCode() - first.GetCode();
    VIXL_ASSERT(IsSingleSpaced() || IsMultiple(range, 2));
    length_ = (IsDoubleSpaced() ? (range / 2) : range) + 1;

    VIXL_ASSERT(length_ <= 4);
  }
  NeonRegisterList(DRegister first,
                   DRegister last,
                   SpacingType spacing,
                   int lane)
      : first_(first.GetCode()),
        spacing_(spacing),
        type_(kOneLane),
        lane_(lane) {
    VIXL_ASSERT((lane >= 0) && (lane < 8));
    VIXL_ASSERT(first.GetCode() <= last.GetCode());

    int range = last.GetCode() - first.GetCode();
    VIXL_ASSERT(IsSingleSpaced() || IsMultiple(range, 2));
    length_ = (IsDoubleSpaced() ? (range / 2) : range) + 1;

    VIXL_ASSERT(length_ <= 4);
  }
  DRegister GetDRegister(int n) const {
    VIXL_ASSERT(n >= 0);
    VIXL_ASSERT(n < length_);
    unsigned code = first_.GetCode() + (IsDoubleSpaced() ? (2 * n) : n);
    VIXL_ASSERT(code < kMaxNumberOfDRegisters);
    return DRegister(code);
  }
  const DRegister& GetFirstDRegister() const { return first_; }
  DRegister GetLastDRegister() const { return GetDRegister(length_ - 1); }
  int GetLength() const { return length_; }
  bool IsSingleSpaced() const { return spacing_ == kSingle; }
  bool IsDoubleSpaced() const { return spacing_ == kDouble; }
  bool IsTransferAllLanes() const { return type_ == kAllLanes; }
  bool IsTransferOneLane() const { return type_ == kOneLane; }
  bool IsTransferMultipleLanes() const { return type_ == kMultipleLanes; }
  int GetTransferLane() const { return lane_; }
};

std::ostream& operator<<(std::ostream& os, NeonRegisterList registers);

enum SpecialRegisterType { APSR = 0, CPSR = 0, SPSR = 1 };

class SpecialRegister {
  uint32_t reg_;

 public:
  explicit SpecialRegister(uint32_t reg) : reg_(reg) {}
  SpecialRegister(SpecialRegisterType reg)  // NOLINT(runtime/explicit)
      : reg_(reg) {}
  uint32_t GetReg() const { return reg_; }
  const char* GetName() const;
  bool Is(SpecialRegister value) const { return reg_ == value.reg_; }
  bool Is(uint32_t value) const { return reg_ == value; }
  bool IsNot(uint32_t value) const { return reg_ != value; }
};

inline std::ostream& operator<<(std::ostream& os, SpecialRegister reg) {
  return os << reg.GetName();
}

enum BankedRegisterType {
  R8_usr = 0x00,
  R9_usr = 0x01,
  R10_usr = 0x02,
  R11_usr = 0x03,
  R12_usr = 0x04,
  SP_usr = 0x05,
  LR_usr = 0x06,
  R8_fiq = 0x08,
  R9_fiq = 0x09,
  R10_fiq = 0x0a,
  R11_fiq = 0x0b,
  R12_fiq = 0x0c,
  SP_fiq = 0x0d,
  LR_fiq = 0x0e,
  LR_irq = 0x10,
  SP_irq = 0x11,
  LR_svc = 0x12,
  SP_svc = 0x13,
  LR_abt = 0x14,
  SP_abt = 0x15,
  LR_und = 0x16,
  SP_und = 0x17,
  LR_mon = 0x1c,
  SP_mon = 0x1d,
  ELR_hyp = 0x1e,
  SP_hyp = 0x1f,
  SPSR_fiq = 0x2e,
  SPSR_irq = 0x30,
  SPSR_svc = 0x32,
  SPSR_abt = 0x34,
  SPSR_und = 0x36,
  SPSR_mon = 0x3c,
  SPSR_hyp = 0x3e
};

class BankedRegister {
  uint32_t reg_;

 public:
  explicit BankedRegister(unsigned reg) : reg_(reg) {}
  BankedRegister(BankedRegisterType reg)  // NOLINT(runtime/explicit)
      : reg_(reg) {}
  uint32_t GetCode() const { return reg_; }
  const char* GetName() const;
};

inline std::ostream& operator<<(std::ostream& os, BankedRegister reg) {
  return os << reg.GetName();
}

enum MaskedSpecialRegisterType {
  APSR_nzcvq = 0x08,
  APSR_g = 0x04,
  APSR_nzcvqg = 0x0c,
  CPSR_c = 0x01,
  CPSR_x = 0x02,
  CPSR_xc = 0x03,
  CPSR_s = APSR_g,
  CPSR_sc = 0x05,
  CPSR_sx = 0x06,
  CPSR_sxc = 0x07,
  CPSR_f = APSR_nzcvq,
  CPSR_fc = 0x09,
  CPSR_fx = 0x0a,
  CPSR_fxc = 0x0b,
  CPSR_fs = APSR_nzcvqg,
  CPSR_fsc = 0x0d,
  CPSR_fsx = 0x0e,
  CPSR_fsxc = 0x0f,
  SPSR_c = 0x11,
  SPSR_x = 0x12,
  SPSR_xc = 0x13,
  SPSR_s = 0x14,
  SPSR_sc = 0x15,
  SPSR_sx = 0x16,
  SPSR_sxc = 0x17,
  SPSR_f = 0x18,
  SPSR_fc = 0x19,
  SPSR_fx = 0x1a,
  SPSR_fxc = 0x1b,
  SPSR_fs = 0x1c,
  SPSR_fsc = 0x1d,
  SPSR_fsx = 0x1e,
  SPSR_fsxc = 0x1f
};

class MaskedSpecialRegister {
  uint32_t reg_;

 public:
  explicit MaskedSpecialRegister(uint32_t reg) : reg_(reg) {
    VIXL_ASSERT(reg <= SPSR_fsxc);
  }
  MaskedSpecialRegister(
      MaskedSpecialRegisterType reg)  // NOLINT(runtime/explicit)
      : reg_(reg) {}
  uint32_t GetReg() const { return reg_; }
  const char* GetName() const;
  bool Is(MaskedSpecialRegister value) const { return reg_ == value.reg_; }
  bool Is(uint32_t value) const { return reg_ == value; }
  bool IsNot(uint32_t value) const { return reg_ != value; }
};

inline std::ostream& operator<<(std::ostream& os, MaskedSpecialRegister reg) {
  return os << reg.GetName();
}

enum SpecialFPRegisterType {
  FPSID = 0x0,
  FPSCR = 0x1,
  MVFR2 = 0x5,
  MVFR1 = 0x6,
  MVFR0 = 0x7,
  FPEXC = 0x8
};

class SpecialFPRegister {
  uint32_t reg_;

 public:
  explicit SpecialFPRegister(uint32_t reg) : reg_(reg) {
#ifdef VIXL_DEBUG
    switch (reg) {
      case FPSID:
      case FPSCR:
      case MVFR2:
      case MVFR1:
      case MVFR0:
      case FPEXC:
        break;
      default:
        VIXL_UNREACHABLE();
    }
#endif
  }
  SpecialFPRegister(SpecialFPRegisterType reg)  // NOLINT(runtime/explicit)
      : reg_(reg) {}
  uint32_t GetReg() const { return reg_; }
  const char* GetName() const;
  bool Is(SpecialFPRegister value) const { return reg_ == value.reg_; }
  bool Is(uint32_t value) const { return reg_ == value; }
  bool IsNot(uint32_t value) const { return reg_ != value; }
};

inline std::ostream& operator<<(std::ostream& os, SpecialFPRegister reg) {
  return os << reg.GetName();
}

class CRegister {
  uint32_t code_;

 public:
  explicit CRegister(uint32_t code) : code_(code) {
    VIXL_ASSERT(code < kNumberOfRegisters);
  }
  uint32_t GetCode() const { return code_; }
  bool Is(CRegister value) const { return code_ == value.code_; }
};

inline std::ostream& operator<<(std::ostream& os, const CRegister reg) {
  return os << "c" << reg.GetCode();
}

// clang-format off
#define CREGISTER_CODE_LIST(R)                                                 \
  R(0)  R(1)  R(2)  R(3)  R(4)  R(5)  R(6)  R(7)                               \
  R(8)  R(9)  R(10) R(11) R(12) R(13) R(14) R(15)
// clang-format on
#define DEFINE_CREGISTER(N) const CRegister c##N(N);
CREGISTER_CODE_LIST(DEFINE_CREGISTER)

enum CoprocessorName { p10 = 10, p11 = 11, p14 = 14, p15 = 15 };

class Coprocessor {
  uint32_t coproc_;

 public:
  explicit Coprocessor(uint32_t coproc) : coproc_(coproc) {}
  Coprocessor(CoprocessorName coproc)  // NOLINT(runtime/explicit)
      : coproc_(static_cast<uint32_t>(coproc)) {}
  bool Is(Coprocessor coproc) const { return coproc_ == coproc.coproc_; }
  bool Is(CoprocessorName coproc) const { return coproc_ == coproc; }
  uint32_t GetCoprocessor() const { return coproc_; }
};

inline std::ostream& operator<<(std::ostream& os, Coprocessor coproc) {
  return os << "p" << coproc.GetCoprocessor();
}

enum ConditionType {
  eq = 0,
  ne = 1,
  cs = 2,
  cc = 3,
  mi = 4,
  pl = 5,
  vs = 6,
  vc = 7,
  hi = 8,
  ls = 9,
  ge = 10,
  lt = 11,
  gt = 12,
  le = 13,
  al = 14,
  hs = cs,
  lo = cc
};

class Condition {
  uint32_t condition_;
  static const uint32_t kNever = 15;
  static const uint32_t kMask = 0xf;
  static const uint32_t kNone = 0x10 | al;

 public:
  static const Condition None() { return Condition(kNone); }
  static const Condition Never() { return Condition(kNever); }
  explicit Condition(uint32_t condition) : condition_(condition) {
    VIXL_ASSERT(condition <= kNone);
  }
  // Users should be able to use "eq", "ne" and so forth to instantiate this
  // class.
  Condition(ConditionType condition)  // NOLINT(runtime/explicit)
      : condition_(condition) {}
  uint32_t GetCondition() const { return condition_ & kMask; }
  bool IsNone() const { return condition_ == kNone; }
  const char* GetName() const;
  bool Is(Condition value) const { return condition_ == value.condition_; }
  bool Is(uint32_t value) const { return condition_ == value; }
  bool IsNot(uint32_t value) const { return condition_ != value; }
  bool IsNever() const { return condition_ == kNever; }
  bool IsNotNever() const { return condition_ != kNever; }
  Condition Negate() const {
    VIXL_ASSERT(IsNot(al) && IsNot(kNever));
    return Condition(condition_ ^ 1);
  }
};

inline std::ostream& operator<<(std::ostream& os, Condition condition) {
  return os << condition.GetName();
}

enum SignType { plus, minus };

class Sign {
 public:
  Sign() : sign_(plus) {}
  Sign(SignType sign) : sign_(sign) {}  // NOLINT(runtime/explicit)
  const char* GetName() const { return (IsPlus() ? "" : "-"); }
  bool IsPlus() const { return sign_ == plus; }
  bool IsMinus() const { return sign_ == minus; }
  int32_t ApplyTo(uint32_t value) { return IsPlus() ? value : -value; }

 private:
  SignType sign_;
};

inline std::ostream& operator<<(std::ostream& os, Sign sign) {
  return os << sign.GetName();
}

enum ShiftType { LSL = 0x0, LSR = 0x1, ASR = 0x2, ROR = 0x3, RRX = 0x4 };

class Shift {
 public:
  Shift() : shift_(LSL) {}
  Shift(ShiftType shift) : shift_(shift) {}  // NOLINT(runtime/explicit)
  explicit Shift(uint32_t shift) : shift_(static_cast<ShiftType>(shift)) {}
  const Shift& GetShift() const { return *this; }
  ShiftType GetType() const { return shift_; }
  uint32_t GetValue() const { return shift_; }
  const char* GetName() const;
  bool IsLSL() const { return shift_ == LSL; }
  bool IsLSR() const { return shift_ == LSR; }
  bool IsASR() const { return shift_ == ASR; }
  bool IsROR() const { return shift_ == ROR; }
  bool IsRRX() const { return shift_ == RRX; }
  bool Is(Shift value) const { return shift_ == value.shift_; }
  bool IsNot(Shift value) const { return shift_ != value.shift_; }
  bool IsValidAmount(uint32_t amount) const;
  static const Shift NoShift;

 protected:
  void SetType(ShiftType s) { shift_ = s; }

 private:
  ShiftType shift_;
};

inline std::ostream& operator<<(std::ostream& os, Shift shift) {
  return os << shift.GetName();
}

class ImmediateShiftOperand : public Shift {
 public:
  // Constructor used for assembly.
  ImmediateShiftOperand(Shift shift, uint32_t amount)
      : Shift(shift), amount_(amount) {
#ifdef VIXL_DEBUG
    switch (shift.GetType()) {
      case LSL:
        VIXL_ASSERT(amount <= 31);
        break;
      case ROR:
        VIXL_ASSERT(amount > 0);
        VIXL_ASSERT(amount <= 31);
        break;
      case LSR:
      case ASR:
        VIXL_ASSERT(amount > 0);
        VIXL_ASSERT(amount <= 32);
        break;
      case RRX:
        VIXL_ASSERT(amount == 0);
        break;
      default:
        VIXL_UNREACHABLE();
        break;
    }
#endif
  }
  // Constructor used for disassembly.
  ImmediateShiftOperand(int shift, int amount);
  uint32_t GetAmount() const { return amount_; }
  bool Is(const ImmediateShiftOperand& rhs) const {
    return amount_ == (rhs.amount_) && Shift::Is(*this);
  }

 private:
  uint32_t amount_;
};

inline std::ostream& operator<<(std::ostream& os,
                                ImmediateShiftOperand const& shift_operand) {
  if (shift_operand.IsLSL() && shift_operand.GetAmount() == 0) return os;
  if (shift_operand.IsRRX()) return os << ", rrx";
  return os << ", " << shift_operand.GetName() << " #"
            << shift_operand.GetAmount();
}

class RegisterShiftOperand : public Shift {
 public:
  RegisterShiftOperand(ShiftType shift, Register shift_register)
      : Shift(shift), shift_register_(shift_register) {
    VIXL_ASSERT(!IsRRX() && shift_register_.IsValid());
  }
  const Register GetShiftRegister() const { return shift_register_; }
  bool Is(const RegisterShiftOperand& rhs) const {
    return shift_register_.Is(rhs.shift_register_) && Shift::Is(*this);
  }

 private:
  Register shift_register_;
};

inline std::ostream& operator<<(std::ostream& s,
                                const RegisterShiftOperand& shift_operand) {
  return s << shift_operand.GetName() << " "
           << shift_operand.GetShiftRegister();
}

enum EncodingSizeType { Best, Narrow, Wide };

class EncodingSize {
  uint32_t size_;

 public:
  explicit EncodingSize(uint32_t size) : size_(size) {}
  EncodingSize(EncodingSizeType size)  // NOLINT(runtime/explicit)
      : size_(size) {}
  uint32_t GetSize() const { return size_; }
  const char* GetName() const;
  bool IsBest() const { return size_ == Best; }
  bool IsNarrow() const { return size_ == Narrow; }
  bool IsWide() const { return size_ == Wide; }
};

inline std::ostream& operator<<(std::ostream& os, EncodingSize size) {
  return os << size.GetName();
}

enum WriteBackValue { NO_WRITE_BACK, WRITE_BACK };

class WriteBack {
  WriteBackValue value_;

 public:
  WriteBack(WriteBackValue value)  // NOLINT(runtime/explicit)
      : value_(value) {}
  explicit WriteBack(int value)
      : value_((value == 0) ? NO_WRITE_BACK : WRITE_BACK) {}
  uint32_t GetWriteBackUint32() const { return (value_ == WRITE_BACK) ? 1 : 0; }
  bool DoesWriteBack() const { return value_ == WRITE_BACK; }
};

inline std::ostream& operator<<(std::ostream& os, WriteBack write_back) {
  if (write_back.DoesWriteBack()) return os << "!";
  return os;
}

class EncodingValue {
  bool valid_;
  uint32_t encoding_value_;

 public:
  EncodingValue() {
    valid_ = false;
    encoding_value_ = 0;
  }
  bool IsValid() const { return valid_; }
  uint32_t GetEncodingValue() const { return encoding_value_; }
  void SetEncodingValue(uint32_t encoding_value) {
    valid_ = true;
    encoding_value_ = encoding_value;
  }
};

class EncodingValueAndImmediate : public EncodingValue {
  uint32_t encoded_immediate_;

 public:
  EncodingValueAndImmediate() { encoded_immediate_ = 0; }
  uint32_t GetEncodedImmediate() const { return encoded_immediate_; }
  void SetEncodedImmediate(uint32_t encoded_immediate) {
    encoded_immediate_ = encoded_immediate;
  }
};

class ImmediateT32 : public EncodingValue {
 public:
  explicit ImmediateT32(uint32_t imm);
  static bool IsImmediateT32(uint32_t imm);
  static uint32_t Decode(uint32_t value);
};

class ImmediateA32 : public EncodingValue {
 public:
  explicit ImmediateA32(uint32_t imm);
  static bool IsImmediateA32(uint32_t imm);
  static uint32_t Decode(uint32_t value);
};

// Return the encoding value of a shift type.
uint32_t TypeEncodingValue(Shift shift);
// Return the encoding value for a shift amount depending on the shift type.
uint32_t AmountEncodingValue(Shift shift, uint32_t amount);

enum MemoryBarrierType {
  OSHLD = 0x1,
  OSHST = 0x2,
  OSH = 0x3,
  NSHLD = 0x5,
  NSHST = 0x6,
  NSH = 0x7,
  ISHLD = 0x9,
  ISHST = 0xa,
  ISH = 0xb,
  LD = 0xd,
  ST = 0xe,
  SY = 0xf
};

class MemoryBarrier {
  MemoryBarrierType type_;

 public:
  MemoryBarrier(MemoryBarrierType type)  // NOLINT(runtime/explicit)
      : type_(type) {}
  MemoryBarrier(uint32_t type)  // NOLINT(runtime/explicit)
      : type_(static_cast<MemoryBarrierType>(type)) {
    VIXL_ASSERT((type & 0x3) != 0);
  }
  MemoryBarrierType GetType() const { return type_; }
  const char* GetName() const;
};

inline std::ostream& operator<<(std::ostream& os, MemoryBarrier option) {
  return os << option.GetName();
}

enum InterruptFlagsType {
  F = 0x1,
  I = 0x2,
  IF = 0x3,
  A = 0x4,
  AF = 0x5,
  AI = 0x6,
  AIF = 0x7
};

class InterruptFlags {
  InterruptFlagsType type_;

 public:
  InterruptFlags(InterruptFlagsType type)  // NOLINT(runtime/explicit)
      : type_(type) {}
  InterruptFlags(uint32_t type)  // NOLINT(runtime/explicit)
      : type_(static_cast<InterruptFlagsType>(type)) {
    VIXL_ASSERT(type <= 7);
  }
  InterruptFlagsType GetType() const { return type_; }
  const char* GetName() const;
};

inline std::ostream& operator<<(std::ostream& os, InterruptFlags option) {
  return os << option.GetName();
}

enum EndiannessType { LE = 0, BE = 1 };

class Endianness {
  EndiannessType type_;

 public:
  Endianness(EndiannessType type) : type_(type) {}  // NOLINT(runtime/explicit)
  Endianness(uint32_t type)                         // NOLINT(runtime/explicit)
      : type_(static_cast<EndiannessType>(type)) {
    VIXL_ASSERT(type <= 1);
  }
  EndiannessType GetType() const { return type_; }
  const char* GetName() const;
};

inline std::ostream& operator<<(std::ostream& os, Endianness endian_specifier) {
  return os << endian_specifier.GetName();
}

enum AlignmentType {
  k16BitAlign = 0,
  k32BitAlign = 1,
  k64BitAlign = 2,
  k128BitAlign = 3,
  k256BitAlign = 4,
  kNoAlignment = 5,
  kBadAlignment = 6
};

class Alignment {
  AlignmentType align_;

 public:
  Alignment(AlignmentType align)  // NOLINT(runtime/explicit)
      : align_(align) {}
  Alignment(uint32_t align)  // NOLINT(runtime/explicit)
      : align_(static_cast<AlignmentType>(align)) {
    VIXL_ASSERT(align <= static_cast<uint32_t>(k256BitAlign));
  }
  AlignmentType GetType() const { return align_; }
  bool Is(AlignmentType type) { return align_ == type; }
};

inline std::ostream& operator<<(std::ostream& os, Alignment align) {
  if (align.GetType() == kBadAlignment) return os << " :??";
  if (align.GetType() == kNoAlignment) return os;
  return os << " :" << (0x10 << static_cast<uint32_t>(align.GetType()));
}

// Structure containing information on forward references.
struct ReferenceInfo {
  int size;
  int min_offset;
  int max_offset;
  int alignment;  // As a power of two.
  enum { kAlignPc, kDontAlignPc } pc_needs_aligning;
};

}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_AARCH32_INSTRUCTIONS_AARCH32_H_
