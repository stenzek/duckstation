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

#ifndef VIXL_AARCH32_OPERANDS_AARCH32_H_
#define VIXL_AARCH32_OPERANDS_AARCH32_H_

#include "aarch32/instructions-aarch32.h"

namespace vixl {
namespace aarch32 {

// Operand represents generic set of arguments to pass to an instruction.
//
//   Usage: <instr> <Rd> , <Operand>
//
//   where <instr> is the instruction to use (e.g., Mov(), Rsb(), etc.)
//         <Rd> is the destination register
//         <Operand> is the rest of the arguments to the instruction
//
//   <Operand> can be one of:
//
//   #<imm> - an unsigned 32-bit immediate value
//   <Rm>, <shift> <#amount> - immediate shifted register
//   <Rm>, <shift> <Rs> - register shifted register
//
class Operand {
 public:
  // { #<immediate> }
  // where <immediate> is uint32_t.
  // This is allowed to be an implicit constructor because Operand is
  // a wrapper class that doesn't normally perform any type conversion.
  Operand(uint32_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoReg),
        shift_(LSL),
        amount_(0),
        rs_(NoReg) {}
  Operand(int32_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoReg),
        shift_(LSL),
        amount_(0),
        rs_(NoReg) {}

  // rm
  // where rm is the base register
  // This is allowed to be an implicit constructor because Operand is
  // a wrapper class that doesn't normally perform any type conversion.
  Operand(Register rm)  // NOLINT(runtime/explicit)
      : imm_(0),
        rm_(rm),
        shift_(LSL),
        amount_(0),
        rs_(NoReg) {
    VIXL_ASSERT(rm_.IsValid());
  }

  // rm, <shift>
  // where rm is the base register, and
  //       <shift> is RRX
  Operand(Register rm, Shift shift)
      : imm_(0), rm_(rm), shift_(shift), amount_(0), rs_(NoReg) {
    VIXL_ASSERT(rm_.IsValid());
    VIXL_ASSERT(shift_.IsRRX());
  }

  // rm, <shift> #<amount>
  // where rm is the base register, and
  //       <shift> is one of {LSL, LSR, ASR, ROR}, and
  //       <amount> is uint6_t.
  Operand(Register rm, Shift shift, uint32_t amount)
      : imm_(0), rm_(rm), shift_(shift), amount_(amount), rs_(NoReg) {
    VIXL_ASSERT(rm_.IsValid());
    VIXL_ASSERT(!shift_.IsRRX());
#ifdef VIXL_DEBUG
    switch (shift_.GetType()) {
      case LSL:
        VIXL_ASSERT(amount_ <= 31);
        break;
      case ROR:
        VIXL_ASSERT(amount_ <= 31);
        break;
      case LSR:
      case ASR:
        VIXL_ASSERT(amount_ <= 32);
        break;
      case RRX:
      default:
        VIXL_UNREACHABLE();
        break;
    }
#endif
  }

  // rm, <shift> rs
  // where rm is the base register, and
  //       <shift> is one of {LSL, LSR, ASR, ROR}, and
  //       rs is the shifted register
  Operand(Register rm, Shift shift, Register rs)
      : imm_(0), rm_(rm), shift_(shift), amount_(0), rs_(rs) {
    VIXL_ASSERT(rm_.IsValid() && rs_.IsValid());
    VIXL_ASSERT(!shift_.IsRRX());
  }

  // Factory methods creating operands from any integral or pointer type. The
  // source must fit into 32 bits.
  template <typename T>
  static Operand From(T immediate) {
#if __cplusplus >= 201103L
    VIXL_STATIC_ASSERT_MESSAGE(std::is_integral<T>::value,
                               "An integral type is required to build an "
                               "immediate operand.");
#endif
    // Allow both a signed or unsigned 32 bit integer to be passed, but store it
    // as a uint32_t. The signedness information will be lost. We have to add a
    // static_cast to make sure the compiler does not complain about implicit 64
    // to 32 narrowing. It's perfectly acceptable for the user to pass a 64-bit
    // value, as long as it can be encoded in 32 bits.
    VIXL_ASSERT(IsInt32(immediate) || IsUint32(immediate));
    return Operand(static_cast<uint32_t>(immediate));
  }

  template <typename T>
  static Operand From(T* address) {
    uintptr_t address_as_integral = reinterpret_cast<uintptr_t>(address);
    VIXL_ASSERT(IsUint32(address_as_integral));
    return Operand(static_cast<uint32_t>(address_as_integral));
  }

  bool IsImmediate() const { return !rm_.IsValid(); }

  bool IsPlainRegister() const {
    return rm_.IsValid() && !shift_.IsRRX() && !rs_.IsValid() && (amount_ == 0);
  }

  bool IsImmediateShiftedRegister() const {
    return rm_.IsValid() && !rs_.IsValid();
  }

  bool IsRegisterShiftedRegister() const {
    return rm_.IsValid() && rs_.IsValid();
  }

  uint32_t GetImmediate() const {
    VIXL_ASSERT(IsImmediate());
    return imm_;
  }

  int32_t GetSignedImmediate() const {
    VIXL_ASSERT(IsImmediate());
    int32_t result;
    memcpy(&result, &imm_, sizeof(result));
    return result;
  }

  Register GetBaseRegister() const {
    VIXL_ASSERT(IsImmediateShiftedRegister() || IsRegisterShiftedRegister());
    return rm_;
  }

  Shift GetShift() const {
    VIXL_ASSERT(IsImmediateShiftedRegister() || IsRegisterShiftedRegister());
    return shift_;
  }

  uint32_t GetShiftAmount() const {
    VIXL_ASSERT(IsImmediateShiftedRegister());
    return amount_;
  }

  Register GetShiftRegister() const {
    VIXL_ASSERT(IsRegisterShiftedRegister());
    return rs_;
  }

  uint32_t GetTypeEncodingValue() const {
    return shift_.IsRRX() ? kRRXEncodedValue : shift_.GetValue();
  }

 private:
// Forbid implicitely creating operands around types that cannot be encoded
// into a uint32_t without loss.
#if __cplusplus >= 201103L
  Operand(int64_t) = delete;   // NOLINT(runtime/explicit)
  Operand(uint64_t) = delete;  // NOLINT(runtime/explicit)
  Operand(float) = delete;     // NOLINT(runtime/explicit)
  Operand(double) = delete;    // NOLINT(runtime/explicit)
#else
  VIXL_NO_RETURN_IN_DEBUG_MODE Operand(int64_t) {  // NOLINT(runtime/explicit)
    VIXL_UNREACHABLE();
  }
  VIXL_NO_RETURN_IN_DEBUG_MODE Operand(uint64_t) {  // NOLINT(runtime/explicit)
    VIXL_UNREACHABLE();
  }
  VIXL_NO_RETURN_IN_DEBUG_MODE Operand(float) {  // NOLINT
    VIXL_UNREACHABLE();
  }
  VIXL_NO_RETURN_IN_DEBUG_MODE Operand(double) {  // NOLINT
    VIXL_UNREACHABLE();
  }
#endif

  uint32_t imm_;
  Register rm_;
  Shift shift_;
  uint32_t amount_;
  Register rs_;
};

std::ostream& operator<<(std::ostream& os, const Operand& operand);

class NeonImmediate {
  template <typename T>
  struct DataTypeIdentity {
    T data_type_;
  };

 public:
  // { #<immediate> }
  // where <immediate> is 32 bit number.
  // This is allowed to be an implicit constructor because NeonImmediate is
  // a wrapper class that doesn't normally perform any type conversion.
  NeonImmediate(uint32_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        immediate_type_(I32) {}
  NeonImmediate(int immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        immediate_type_(I32) {}

  // { #<immediate> }
  // where <immediate> is a 64 bit number
  // This is allowed to be an implicit constructor because NeonImmediate is
  // a wrapper class that doesn't normally perform any type conversion.
  NeonImmediate(int64_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        immediate_type_(I64) {}
  NeonImmediate(uint64_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        immediate_type_(I64) {}

  // { #<immediate> }
  // where <immediate> is a non zero floating point number which can be encoded
  // as an 8 bit floating point (checked by the constructor).
  // This is allowed to be an implicit constructor because NeonImmediate is
  // a wrapper class that doesn't normally perform any type conversion.
  NeonImmediate(float immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        immediate_type_(F32) {}
  NeonImmediate(double immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        immediate_type_(F64) {}

  NeonImmediate(const NeonImmediate& src)
      : imm_(src.imm_), immediate_type_(src.immediate_type_) {}

  template <typename T>
  T GetImmediate() const {
    return GetImmediate(DataTypeIdentity<T>());
  }

  template <typename T>
  T GetImmediate(const DataTypeIdentity<T>&) const {
    VIXL_ASSERT(sizeof(T) <= sizeof(uint32_t));
    VIXL_ASSERT(CanConvert<T>());
    if (immediate_type_.Is(I64))
      return static_cast<T>(imm_.u64_ & static_cast<T>(-1));
    if (immediate_type_.Is(F64) || immediate_type_.Is(F32)) return 0;
    return static_cast<T>(imm_.u32_ & static_cast<T>(-1));
  }

  uint64_t GetImmediate(const DataTypeIdentity<uint64_t>&) const {
    VIXL_ASSERT(CanConvert<uint64_t>());
    if (immediate_type_.Is(I32)) return imm_.u32_;
    if (immediate_type_.Is(F64) || immediate_type_.Is(F32)) return 0;
    return imm_.u64_;
  }
  float GetImmediate(const DataTypeIdentity<float>&) const {
    VIXL_ASSERT(CanConvert<float>());
    if (immediate_type_.Is(F64)) return static_cast<float>(imm_.d_);
    return imm_.f_;
  }
  double GetImmediate(const DataTypeIdentity<double>&) const {
    VIXL_ASSERT(CanConvert<double>());
    if (immediate_type_.Is(F32)) return static_cast<double>(imm_.f_);
    return imm_.d_;
  }

  bool IsInteger32() const { return immediate_type_.Is(I32); }
  bool IsInteger64() const { return immediate_type_.Is(I64); }
  bool IsInteger() const { return IsInteger32() | IsInteger64(); }
  bool IsFloat() const { return immediate_type_.Is(F32); }
  bool IsDouble() const { return immediate_type_.Is(F64); }
  bool IsFloatZero() const {
    if (immediate_type_.Is(F32)) return imm_.f_ == 0.0f;
    if (immediate_type_.Is(F64)) return imm_.d_ == 0.0;
    return false;
  }

  template <typename T>
  bool CanConvert() const {
    return CanConvert(DataTypeIdentity<T>());
  }

  template <typename T>
  bool CanConvert(const DataTypeIdentity<T>&) const {
    VIXL_ASSERT(sizeof(T) < sizeof(uint32_t));
    return (immediate_type_.Is(I32) && ((imm_.u32_ >> (8 * sizeof(T))) == 0)) ||
           (immediate_type_.Is(I64) && ((imm_.u64_ >> (8 * sizeof(T))) == 0)) ||
           (immediate_type_.Is(F32) && (imm_.f_ == 0.0f)) ||
           (immediate_type_.Is(F64) && (imm_.d_ == 0.0));
  }
  bool CanConvert(const DataTypeIdentity<uint32_t>&) const {
    return immediate_type_.Is(I32) ||
           (immediate_type_.Is(I64) && ((imm_.u64_ >> 32) == 0)) ||
           (immediate_type_.Is(F32) && (imm_.f_ == 0.0f)) ||
           (immediate_type_.Is(F64) && (imm_.d_ == 0.0));
  }
  bool CanConvert(const DataTypeIdentity<uint64_t>&) const {
    return IsInteger() || CanConvert<uint32_t>();
  }
  bool CanConvert(const DataTypeIdentity<float>&) const {
    return IsFloat() || IsDouble();
  }
  bool CanConvert(const DataTypeIdentity<double>&) const {
    return IsFloat() || IsDouble();
  }
  friend std::ostream& operator<<(std::ostream& os,
                                  const NeonImmediate& operand);

 private:
  union NeonImmediateType {
    uint64_t u64_;
    double d_;
    uint32_t u32_;
    float f_;
    NeonImmediateType(uint64_t u) : u64_(u) {}
    NeonImmediateType(int64_t u) : u64_(u) {}
    NeonImmediateType(uint32_t u) : u32_(u) {}
    NeonImmediateType(int32_t u) : u32_(u) {}
    NeonImmediateType(double d) : d_(d) {}
    NeonImmediateType(float f) : f_(f) {}
    NeonImmediateType(const NeonImmediateType& ref) : u64_(ref.u64_) {}
  } imm_;

  DataType immediate_type_;
};

std::ostream& operator<<(std::ostream& os, const NeonImmediate& operand);

class NeonOperand {
 public:
  NeonOperand(int32_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoDReg) {}
  NeonOperand(uint32_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoDReg) {}
  NeonOperand(int64_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoDReg) {}
  NeonOperand(uint64_t immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoDReg) {}
  NeonOperand(float immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoDReg) {}
  NeonOperand(double immediate)  // NOLINT(runtime/explicit)
      : imm_(immediate),
        rm_(NoDReg) {}
  NeonOperand(const NeonImmediate& imm)  // NOLINT(runtime/explicit)
      : imm_(imm),
        rm_(NoDReg) {}
  NeonOperand(const VRegister& rm)  // NOLINT(runtime/explicit)
      : imm_(0),
        rm_(rm) {
    VIXL_ASSERT(rm_.IsValid());
  }

  bool IsImmediate() const { return !rm_.IsValid(); }
  bool IsRegister() const { return rm_.IsValid(); }
  bool IsFloatZero() const {
    VIXL_ASSERT(IsImmediate());
    return imm_.IsFloatZero();
  }

  const NeonImmediate& GetNeonImmediate() const { return imm_; }

  VRegister GetRegister() const {
    VIXL_ASSERT(IsRegister());
    return rm_;
  }

 protected:
  NeonImmediate imm_;
  VRegister rm_;
};

std::ostream& operator<<(std::ostream& os, const NeonOperand& operand);

// SOperand represents either an immediate or a SRegister.
class SOperand : public NeonOperand {
 public:
  // #<immediate>
  // where <immediate> is 32bit int
  // This is allowed to be an implicit constructor because SOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  SOperand(int32_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  SOperand(uint32_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  // #<immediate>
  // where <immediate> is 32bit float
  SOperand(float immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  // where <immediate> is 64bit float
  SOperand(double immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}

  SOperand(const NeonImmediate& imm)  // NOLINT(runtime/explicit)
      : NeonOperand(imm) {}

  // rm
  // This is allowed to be an implicit constructor because SOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  SOperand(SRegister rm)  // NOLINT(runtime/explicit)
      : NeonOperand(rm) {}
  SRegister GetRegister() const {
    VIXL_ASSERT(IsRegister() && (rm_.GetType() == CPURegister::kSRegister));
    return SRegister(rm_.GetCode());
  }
};

// DOperand represents either an immediate or a DRegister.
std::ostream& operator<<(std::ostream& os, const SOperand& operand);

class DOperand : public NeonOperand {
 public:
  // #<immediate>
  // where <immediate> is uint32_t.
  // This is allowed to be an implicit constructor because DOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  DOperand(int32_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  DOperand(uint32_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  DOperand(int64_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  DOperand(uint64_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}

  // #<immediate>
  // where <immediate> is a non zero floating point number which can be encoded
  // as an 8 bit floating point (checked by the constructor).
  // This is allowed to be an implicit constructor because DOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  DOperand(float immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  DOperand(double immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}

  DOperand(const NeonImmediate& imm)  // NOLINT(runtime/explicit)
      : NeonOperand(imm) {}
  // rm
  // This is allowed to be an implicit constructor because DOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  DOperand(DRegister rm)  // NOLINT(runtime/explicit)
      : NeonOperand(rm) {}

  DRegister GetRegister() const {
    VIXL_ASSERT(IsRegister() && (rm_.GetType() == CPURegister::kDRegister));
    return DRegister(rm_.GetCode());
  }
};

std::ostream& operator<<(std::ostream& os, const DOperand& operand);

// QOperand represents either an immediate or a QRegister.
class QOperand : public NeonOperand {
 public:
  // #<immediate>
  // where <immediate> is uint32_t.
  // This is allowed to be an implicit constructor because QOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  QOperand(int32_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  QOperand(uint32_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  QOperand(int64_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  QOperand(uint64_t immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  QOperand(float immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}
  QOperand(double immediate)  // NOLINT(runtime/explicit)
      : NeonOperand(immediate) {}

  QOperand(const NeonImmediate& imm)  // NOLINT(runtime/explicit)
      : NeonOperand(imm) {}

  // rm
  // This is allowed to be an implicit constructor because QOperand is
  // a wrapper class that doesn't normally perform any type conversion.
  QOperand(QRegister rm)  // NOLINT(runtime/explicit)
      : NeonOperand(rm) {
    VIXL_ASSERT(rm_.IsValid());
  }

  QRegister GetRegister() const {
    VIXL_ASSERT(IsRegister() && (rm_.GetType() == CPURegister::kQRegister));
    return QRegister(rm_.GetCode());
  }
};

std::ostream& operator<<(std::ostream& os, const QOperand& operand);

class ImmediateVFP : public EncodingValue {
  template <typename T>
  struct FloatType {
    typedef T base_type;
  };

 public:
  explicit ImmediateVFP(const NeonImmediate& neon_imm) {
    if (neon_imm.IsFloat()) {
      const float imm = neon_imm.GetImmediate<float>();
      if (VFP::IsImmFP32(imm)) {
        SetEncodingValue(VFP::FP32ToImm8(imm));
      }
    } else if (neon_imm.IsDouble()) {
      const double imm = neon_imm.GetImmediate<double>();
      if (VFP::IsImmFP64(imm)) {
        SetEncodingValue(VFP::FP64ToImm8(imm));
      }
    }
  }

  template <typename T>
  static T Decode(uint32_t v) {
    return Decode(v, FloatType<T>());
  }

  static float Decode(uint32_t imm8, const FloatType<float>&) {
    return VFP::Imm8ToFP32(imm8);
  }

  static double Decode(uint32_t imm8, const FloatType<double>&) {
    return VFP::Imm8ToFP64(imm8);
  }
};


class ImmediateVbic : public EncodingValueAndImmediate {
 public:
  ImmediateVbic(DataType dt, const NeonImmediate& neon_imm);
  static DataType DecodeDt(uint32_t cmode);
  static NeonImmediate DecodeImmediate(uint32_t cmode, uint32_t immediate);
};

class ImmediateVand : public ImmediateVbic {
 public:
  ImmediateVand(DataType dt, const NeonImmediate neon_imm)
      : ImmediateVbic(dt, neon_imm) {
    if (IsValid()) {
      SetEncodedImmediate(~GetEncodedImmediate() & 0xff);
    }
  }
};

class ImmediateVmov : public EncodingValueAndImmediate {
 public:
  ImmediateVmov(DataType dt, const NeonImmediate& neon_imm);
  static DataType DecodeDt(uint32_t cmode);
  static NeonImmediate DecodeImmediate(uint32_t cmode, uint32_t immediate);
};

class ImmediateVmvn : public EncodingValueAndImmediate {
 public:
  ImmediateVmvn(DataType dt, const NeonImmediate& neon_imm);
  static DataType DecodeDt(uint32_t cmode);
  static NeonImmediate DecodeImmediate(uint32_t cmode, uint32_t immediate);
};

class ImmediateVorr : public EncodingValueAndImmediate {
 public:
  ImmediateVorr(DataType dt, const NeonImmediate& neon_imm);
  static DataType DecodeDt(uint32_t cmode);
  static NeonImmediate DecodeImmediate(uint32_t cmode, uint32_t immediate);
};

class ImmediateVorn : public ImmediateVorr {
 public:
  ImmediateVorn(DataType dt, const NeonImmediate& neon_imm)
      : ImmediateVorr(dt, neon_imm) {
    if (IsValid()) {
      SetEncodedImmediate(~GetEncodedImmediate() & 0xff);
    }
  }
};

// MemOperand represents the addressing mode of a load or store instruction.
//
//   Usage: <instr> <Rt> , <MemOperand>
//
//   where <instr> is the instruction to use (e.g., Ldr(), Str(), etc.),
//         <Rt> is general purpose register to be transferred,
//         <MemOperand> is the rest of the arguments to the instruction
//
//   <MemOperand> can be in one of 3 addressing modes:
//
//   [ <Rn>, <offset> ]   ==  offset addressing
//   [ <Rn>, <offset> ]!  ==  pre-indexed addressing
//   [ <Rn> ], <offset>   ==  post-indexed addressing
//
//   where <offset> can be one of:
//     - an immediate constant, such as <imm8>, <imm12>
//     - an index register <Rm>
//     - a shifted index register <Rm>, <shift> #<amount>
//
//   The index register may have an associated {+/-} sign,
//   which if ommitted, defaults to + .
//
//   We have two constructors for the offset:
//
//   One with a signed value offset parameter. The value of sign_ is
//   "sign_of(constructor's offset parameter) and the value of offset_ is
//   "constructor's offset parameter".
//
//   The other with a sign and a positive value offset parameters. The value of
//   sign_ is "constructor's sign parameter" and the value of offset_ is
//   "constructor's sign parameter * constructor's offset parameter".
//
//   The value of offset_ reflects the effective offset. For an offset_ of 0,
//   sign_ can be positive or negative. Otherwise, sign_ always agrees with
//   the sign of offset_.
class MemOperand {
 public:
  // rn
  // where rn is the general purpose base register only
  explicit MemOperand(Register rn, AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(plus),
        rm_(NoReg),
        shift_(LSL),
        shift_amount_(0),
        addrmode_(addrmode | kMemOperandRegisterOnly) {
    VIXL_ASSERT(rn_.IsValid());
  }

  // rn, #<imm>
  // where rn is the general purpose base register,
  //       <imm> is a 32-bit offset to add to rn
  //
  // Note: if rn is PC, then this form is equivalent to a "label"
  // Note: the second constructor allow minus zero (-0).
  MemOperand(Register rn, int32_t offset, AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(offset),
        sign_((offset < 0) ? minus : plus),
        rm_(NoReg),
        shift_(LSL),
        shift_amount_(0),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid());
  }
  MemOperand(Register rn, Sign sign, int32_t offset, AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(sign.IsPlus() ? offset : -offset),
        sign_(sign),
        rm_(NoReg),
        shift_(LSL),
        shift_amount_(0),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid());
    // With this constructor, the sign must only be specified by "sign".
    VIXL_ASSERT(offset >= 0);
  }

  // rn, {+/-}rm
  // where rn is the general purpose base register,
  //       {+/-} is the sign of the index register,
  //       rm is the general purpose index register,
  MemOperand(Register rn, Sign sign, Register rm, AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(sign),
        rm_(rm),
        shift_(LSL),
        shift_amount_(0),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid() && rm_.IsValid());
  }

  // rn, rm
  // where rn is the general purpose base register,
  //       rm is the general purpose index register,
  MemOperand(Register rn, Register rm, AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(plus),
        rm_(rm),
        shift_(LSL),
        shift_amount_(0),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid() && rm_.IsValid());
  }

  // rn, {+/-}rm, <shift>
  // where rn is the general purpose base register,
  //       {+/-} is the sign of the index register,
  //       rm is the general purpose index register,
  //       <shift> is RRX, applied to value from rm
  MemOperand(Register rn,
             Sign sign,
             Register rm,
             Shift shift,
             AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(sign),
        rm_(rm),
        shift_(shift),
        shift_amount_(0),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid() && rm_.IsValid());
    VIXL_ASSERT(shift_.IsRRX());
  }

  // rn, rm, <shift>
  // where rn is the general purpose base register,
  //       rm is the general purpose index register,
  //       <shift> is RRX, applied to value from rm
  MemOperand(Register rn, Register rm, Shift shift, AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(plus),
        rm_(rm),
        shift_(shift),
        shift_amount_(0),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid() && rm_.IsValid());
    VIXL_ASSERT(shift_.IsRRX());
  }

  // rn, {+/-}rm, <shift> #<amount>
  // where rn is the general purpose base register,
  //       {+/-} is the sign of the index register,
  //       rm is the general purpose index register,
  //       <shift> is one of {LSL, LSR, ASR, ROR}, applied to value from rm
  //       <shift_amount> is optional size to apply to value from rm
  MemOperand(Register rn,
             Sign sign,
             Register rm,
             Shift shift,
             uint32_t shift_amount,
             AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(sign),
        rm_(rm),
        shift_(shift),
        shift_amount_(shift_amount),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid() && rm_.IsValid());
    CheckShift();
  }

  // rn, rm, <shift> #<amount>
  // where rn is the general purpose base register,
  //       rm is the general purpose index register,
  //       <shift> is one of {LSL, LSR, ASR, ROR}, applied to value from rm
  //       <shift_amount> is optional size to apply to value from rm
  MemOperand(Register rn,
             Register rm,
             Shift shift,
             uint32_t shift_amount,
             AddrMode addrmode = Offset)
      : rn_(rn),
        offset_(0),
        sign_(plus),
        rm_(rm),
        shift_(shift),
        shift_amount_(shift_amount),
        addrmode_(addrmode) {
    VIXL_ASSERT(rn_.IsValid() && rm_.IsValid());
    CheckShift();
  }

  Register GetBaseRegister() const { return rn_; }
  int32_t GetOffsetImmediate() const { return offset_; }
  bool IsOffsetImmediateWithinRange(int min,
                                    int max,
                                    int multiple_of = 1) const {
    return (offset_ >= min) && (offset_ <= max) &&
           ((offset_ % multiple_of) == 0);
  }
  Sign GetSign() const { return sign_; }
  Register GetOffsetRegister() const { return rm_; }
  Shift GetShift() const { return shift_; }
  unsigned GetShiftAmount() const { return shift_amount_; }
  AddrMode GetAddrMode() const {
    return static_cast<AddrMode>(addrmode_ & kMemOperandAddrModeMask);
  }
  bool IsRegisterOnly() const {
    return (addrmode_ & kMemOperandRegisterOnly) != 0;
  }

  bool IsImmediate() const { return !rm_.IsValid(); }
  bool IsImmediateZero() const { return !rm_.IsValid() && (offset_ == 0); }
  bool IsPlainRegister() const {
    return rm_.IsValid() && shift_.IsLSL() && (shift_amount_ == 0);
  }
  bool IsShiftedRegister() const { return rm_.IsValid(); }
  bool IsImmediateOffset() const {
    return (GetAddrMode() == Offset) && !rm_.IsValid();
  }
  bool IsImmediateZeroOffset() const {
    return (GetAddrMode() == Offset) && !rm_.IsValid() && (offset_ == 0);
  }
  bool IsRegisterOffset() const {
    return (GetAddrMode() == Offset) && rm_.IsValid() && shift_.IsLSL() &&
           (shift_amount_ == 0);
  }
  bool IsShiftedRegisterOffset() const {
    return (GetAddrMode() == Offset) && rm_.IsValid();
  }
  uint32_t GetTypeEncodingValue() const {
    return shift_.IsRRX() ? kRRXEncodedValue : shift_.GetValue();
  }
  bool IsOffset() const { return GetAddrMode() == Offset; }
  bool IsPreIndex() const { return GetAddrMode() == PreIndex; }
  bool IsPostIndex() const { return GetAddrMode() == PostIndex; }
  bool IsShiftValid() const { return shift_.IsValidAmount(shift_amount_); }

 private:
  static const int kMemOperandRegisterOnly = 0x1000;
  static const int kMemOperandAddrModeMask = 0xfff;
  void CheckShift() {
#ifdef VIXL_DEBUG
    // Disallow any zero shift other than RRX #0 and LSL #0 .
    if ((shift_amount_ == 0) && shift_.IsRRX()) return;
    if ((shift_amount_ == 0) && !shift_.IsLSL()) {
      VIXL_ABORT_WITH_MSG(
          "A shift by 0 is only accepted in "
          "the case of lsl and will be treated as "
          "no shift.\n");
    }
    switch (shift_.GetType()) {
      case LSL:
        VIXL_ASSERT(shift_amount_ <= 31);
        break;
      case ROR:
        VIXL_ASSERT(shift_amount_ <= 31);
        break;
      case LSR:
      case ASR:
        VIXL_ASSERT(shift_amount_ <= 32);
        break;
      case RRX:
      default:
        VIXL_UNREACHABLE();
        break;
    }
#endif
  }
  Register rn_;
  int32_t offset_;
  Sign sign_;
  Register rm_;
  Shift shift_;
  uint32_t shift_amount_;
  uint32_t addrmode_;
};

std::ostream& operator<<(std::ostream& os, const MemOperand& operand);

class AlignedMemOperand : public MemOperand {
 public:
  AlignedMemOperand(Register rn, Alignment align, AddrMode addrmode = Offset)
      : MemOperand(rn, addrmode), align_(align) {
    VIXL_ASSERT(addrmode != PreIndex);
  }

  AlignedMemOperand(Register rn,
                    Alignment align,
                    Register rm,
                    AddrMode addrmode)
      : MemOperand(rn, rm, addrmode), align_(align) {
    VIXL_ASSERT(addrmode != PreIndex);
  }

  Alignment GetAlignment() const { return align_; }

 private:
  Alignment align_;
};

std::ostream& operator<<(std::ostream& os, const AlignedMemOperand& operand);

}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_AARCH32_OPERANDS_AARCH32_H_
