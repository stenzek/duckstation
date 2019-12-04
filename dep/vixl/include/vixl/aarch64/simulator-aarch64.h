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

#ifndef VIXL_AARCH64_SIMULATOR_AARCH64_H_
#define VIXL_AARCH64_SIMULATOR_AARCH64_H_

#include <vector>

#include "../globals-vixl.h"
#include "../utils-vixl.h"
#include "../cpu-features.h"

#include "abi-aarch64.h"
#include "cpu-features-auditor-aarch64.h"
#include "disasm-aarch64.h"
#include "instructions-aarch64.h"
#include "instrument-aarch64.h"
#include "simulator-constants-aarch64.h"

#ifdef VIXL_INCLUDE_SIMULATOR_AARCH64

// These are only used for the ABI feature, and depend on checks performed for
// it.
#ifdef VIXL_HAS_ABI_SUPPORT
#include <tuple>
#if __cplusplus >= 201402L
// Required for `std::index_sequence`
#include <utility>
#endif
#endif

namespace vixl {
namespace aarch64 {

// Representation of memory, with typed getters and setters for access.
class Memory {
 public:
  template <typename T>
  static T AddressUntag(T address) {
    // Cast the address using a C-style cast. A reinterpret_cast would be
    // appropriate, but it can't cast one integral type to another.
    uint64_t bits = (uint64_t)address;
    return (T)(bits & ~kAddressTagMask);
  }

  template <typename T, typename A>
  static T Read(A address) {
    T value;
    address = AddressUntag(address);
    VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
                (sizeof(value) == 4) || (sizeof(value) == 8) ||
                (sizeof(value) == 16));
    memcpy(&value, reinterpret_cast<const char*>(address), sizeof(value));
    return value;
  }

  template <typename T, typename A>
  static void Write(A address, T value) {
    address = AddressUntag(address);
    VIXL_ASSERT((sizeof(value) == 1) || (sizeof(value) == 2) ||
                (sizeof(value) == 4) || (sizeof(value) == 8) ||
                (sizeof(value) == 16));
    memcpy(reinterpret_cast<char*>(address), &value, sizeof(value));
  }
};

// Represent a register (r0-r31, v0-v31).
template <int kSizeInBytes>
class SimRegisterBase {
 public:
  SimRegisterBase() : written_since_last_log_(false) {}

  // Write the specified value. The value is zero-extended if necessary.
  template <typename T>
  void Write(T new_value) {
    if (sizeof(new_value) < kSizeInBytes) {
      // All AArch64 registers are zero-extending.
      memset(value_ + sizeof(new_value), 0, kSizeInBytes - sizeof(new_value));
    }
    WriteLane(new_value, 0);
    NotifyRegisterWrite();
  }
  template <typename T>
  VIXL_DEPRECATED("Write", void Set(T new_value)) {
    Write(new_value);
  }

  // Insert a typed value into a register, leaving the rest of the register
  // unchanged. The lane parameter indicates where in the register the value
  // should be inserted, in the range [ 0, sizeof(value_) / sizeof(T) ), where
  // 0 represents the least significant bits.
  template <typename T>
  void Insert(int lane, T new_value) {
    WriteLane(new_value, lane);
    NotifyRegisterWrite();
  }

  // Get the value as the specified type. The value is truncated if necessary.
  template <typename T>
  T Get() const {
    return GetLane<T>(0);
  }

  // Get the lane value as the specified type. The value is truncated if
  // necessary.
  template <typename T>
  T GetLane(int lane) const {
    T result;
    ReadLane(&result, lane);
    return result;
  }
  template <typename T>
  VIXL_DEPRECATED("GetLane", T Get(int lane) const) {
    return GetLane(lane);
  }

  // TODO: Make this return a map of updated bytes, so that we can highlight
  // updated lanes for load-and-insert. (That never happens for scalar code, but
  // NEON has some instructions that can update individual lanes.)
  bool WrittenSinceLastLog() const { return written_since_last_log_; }

  void NotifyRegisterLogged() { written_since_last_log_ = false; }

 protected:
  uint8_t value_[kSizeInBytes];

  // Helpers to aid with register tracing.
  bool written_since_last_log_;

  void NotifyRegisterWrite() { written_since_last_log_ = true; }

 private:
  template <typename T>
  void ReadLane(T* dst, int lane) const {
    VIXL_ASSERT(lane >= 0);
    VIXL_ASSERT((sizeof(*dst) + (lane * sizeof(*dst))) <= kSizeInBytes);
    memcpy(dst, &value_[lane * sizeof(*dst)], sizeof(*dst));
  }

  template <typename T>
  void WriteLane(T src, int lane) {
    VIXL_ASSERT(lane >= 0);
    VIXL_ASSERT((sizeof(src) + (lane * sizeof(src))) <= kSizeInBytes);
    memcpy(&value_[lane * sizeof(src)], &src, sizeof(src));
  }
};
typedef SimRegisterBase<kXRegSizeInBytes> SimRegister;   // r0-r31
typedef SimRegisterBase<kQRegSizeInBytes> SimVRegister;  // v0-v31

// The default ReadLane and WriteLane methods assume what we are copying is
// "trivially copyable" by using memcpy. We have to provide alternative
// implementations for SimFloat16 which cannot be copied this way.

template <>
template <>
inline void SimVRegister::ReadLane(vixl::internal::SimFloat16* dst,
                                   int lane) const {
  uint16_t rawbits;
  ReadLane(&rawbits, lane);
  *dst = RawbitsToFloat16(rawbits);
}

template <>
template <>
inline void SimVRegister::WriteLane(vixl::internal::SimFloat16 src, int lane) {
  WriteLane(Float16ToRawbits(src), lane);
}

// Representation of a vector register, with typed getters and setters for lanes
// and additional information to represent lane state.
class LogicVRegister {
 public:
  inline LogicVRegister(
      SimVRegister& other)  // NOLINT(runtime/references)(runtime/explicit)
      : register_(other) {
    for (size_t i = 0; i < ArrayLength(saturated_); i++) {
      saturated_[i] = kNotSaturated;
    }
    for (size_t i = 0; i < ArrayLength(round_); i++) {
      round_[i] = 0;
    }
  }

  int64_t Int(VectorFormat vform, int index) const {
    int64_t element;
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8:
        element = register_.GetLane<int8_t>(index);
        break;
      case 16:
        element = register_.GetLane<int16_t>(index);
        break;
      case 32:
        element = register_.GetLane<int32_t>(index);
        break;
      case 64:
        element = register_.GetLane<int64_t>(index);
        break;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }
    return element;
  }

  uint64_t Uint(VectorFormat vform, int index) const {
    uint64_t element;
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8:
        element = register_.GetLane<uint8_t>(index);
        break;
      case 16:
        element = register_.GetLane<uint16_t>(index);
        break;
      case 32:
        element = register_.GetLane<uint32_t>(index);
        break;
      case 64:
        element = register_.GetLane<uint64_t>(index);
        break;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }
    return element;
  }

  uint64_t UintLeftJustified(VectorFormat vform, int index) const {
    return Uint(vform, index) << (64 - LaneSizeInBitsFromFormat(vform));
  }

  int64_t IntLeftJustified(VectorFormat vform, int index) const {
    uint64_t value = UintLeftJustified(vform, index);
    int64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
  }

  void SetInt(VectorFormat vform, int index, int64_t value) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8:
        register_.Insert(index, static_cast<int8_t>(value));
        break;
      case 16:
        register_.Insert(index, static_cast<int16_t>(value));
        break;
      case 32:
        register_.Insert(index, static_cast<int32_t>(value));
        break;
      case 64:
        register_.Insert(index, static_cast<int64_t>(value));
        break;
      default:
        VIXL_UNREACHABLE();
        return;
    }
  }

  void SetIntArray(VectorFormat vform, const int64_t* src) const {
    ClearForWrite(vform);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      SetInt(vform, i, src[i]);
    }
  }

  void SetUint(VectorFormat vform, int index, uint64_t value) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8:
        register_.Insert(index, static_cast<uint8_t>(value));
        break;
      case 16:
        register_.Insert(index, static_cast<uint16_t>(value));
        break;
      case 32:
        register_.Insert(index, static_cast<uint32_t>(value));
        break;
      case 64:
        register_.Insert(index, static_cast<uint64_t>(value));
        break;
      default:
        VIXL_UNREACHABLE();
        return;
    }
  }

  void SetUintArray(VectorFormat vform, const uint64_t* src) const {
    ClearForWrite(vform);
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      SetUint(vform, i, src[i]);
    }
  }

  void ReadUintFromMem(VectorFormat vform, int index, uint64_t addr) const {
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8:
        register_.Insert(index, Memory::Read<uint8_t>(addr));
        break;
      case 16:
        register_.Insert(index, Memory::Read<uint16_t>(addr));
        break;
      case 32:
        register_.Insert(index, Memory::Read<uint32_t>(addr));
        break;
      case 64:
        register_.Insert(index, Memory::Read<uint64_t>(addr));
        break;
      default:
        VIXL_UNREACHABLE();
        return;
    }
  }

  void WriteUintToMem(VectorFormat vform, int index, uint64_t addr) const {
    uint64_t value = Uint(vform, index);
    switch (LaneSizeInBitsFromFormat(vform)) {
      case 8:
        Memory::Write(addr, static_cast<uint8_t>(value));
        break;
      case 16:
        Memory::Write(addr, static_cast<uint16_t>(value));
        break;
      case 32:
        Memory::Write(addr, static_cast<uint32_t>(value));
        break;
      case 64:
        Memory::Write(addr, value);
        break;
    }
  }

  template <typename T>
  T Float(int index) const {
    return register_.GetLane<T>(index);
  }

  template <typename T>
  void SetFloat(int index, T value) const {
    register_.Insert(index, value);
  }

  // When setting a result in a register of size less than Q, the top bits of
  // the Q register must be cleared.
  void ClearForWrite(VectorFormat vform) const {
    unsigned size = RegisterSizeInBytesFromFormat(vform);
    for (unsigned i = size; i < kQRegSizeInBytes; i++) {
      SetUint(kFormat16B, i, 0);
    }
  }

  // Saturation state for each lane of a vector.
  enum Saturation {
    kNotSaturated = 0,
    kSignedSatPositive = 1 << 0,
    kSignedSatNegative = 1 << 1,
    kSignedSatMask = kSignedSatPositive | kSignedSatNegative,
    kSignedSatUndefined = kSignedSatMask,
    kUnsignedSatPositive = 1 << 2,
    kUnsignedSatNegative = 1 << 3,
    kUnsignedSatMask = kUnsignedSatPositive | kUnsignedSatNegative,
    kUnsignedSatUndefined = kUnsignedSatMask
  };

  // Getters for saturation state.
  Saturation GetSignedSaturation(int index) {
    return static_cast<Saturation>(saturated_[index] & kSignedSatMask);
  }

  Saturation GetUnsignedSaturation(int index) {
    return static_cast<Saturation>(saturated_[index] & kUnsignedSatMask);
  }

  // Setters for saturation state.
  void ClearSat(int index) { saturated_[index] = kNotSaturated; }

  void SetSignedSat(int index, bool positive) {
    SetSatFlag(index, positive ? kSignedSatPositive : kSignedSatNegative);
  }

  void SetUnsignedSat(int index, bool positive) {
    SetSatFlag(index, positive ? kUnsignedSatPositive : kUnsignedSatNegative);
  }

  void SetSatFlag(int index, Saturation sat) {
    saturated_[index] = static_cast<Saturation>(saturated_[index] | sat);
    VIXL_ASSERT((sat & kUnsignedSatMask) != kUnsignedSatUndefined);
    VIXL_ASSERT((sat & kSignedSatMask) != kSignedSatUndefined);
  }

  // Saturate lanes of a vector based on saturation state.
  LogicVRegister& SignedSaturate(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      Saturation sat = GetSignedSaturation(i);
      if (sat == kSignedSatPositive) {
        SetInt(vform, i, MaxIntFromFormat(vform));
      } else if (sat == kSignedSatNegative) {
        SetInt(vform, i, MinIntFromFormat(vform));
      }
    }
    return *this;
  }

  LogicVRegister& UnsignedSaturate(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      Saturation sat = GetUnsignedSaturation(i);
      if (sat == kUnsignedSatPositive) {
        SetUint(vform, i, MaxUintFromFormat(vform));
      } else if (sat == kUnsignedSatNegative) {
        SetUint(vform, i, 0);
      }
    }
    return *this;
  }

  // Getter for rounding state.
  bool GetRounding(int index) { return round_[index]; }

  // Setter for rounding state.
  void SetRounding(int index, bool round) { round_[index] = round; }

  // Round lanes of a vector based on rounding state.
  LogicVRegister& Round(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      SetUint(vform, i, Uint(vform, i) + (GetRounding(i) ? 1 : 0));
    }
    return *this;
  }

  // Unsigned halve lanes of a vector, and use the saturation state to set the
  // top bit.
  LogicVRegister& Uhalve(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      uint64_t val = Uint(vform, i);
      SetRounding(i, (val & 1) == 1);
      val >>= 1;
      if (GetUnsignedSaturation(i) != kNotSaturated) {
        // If the operation causes unsigned saturation, the bit shifted into the
        // most significant bit must be set.
        val |= (MaxUintFromFormat(vform) >> 1) + 1;
      }
      SetInt(vform, i, val);
    }
    return *this;
  }

  // Signed halve lanes of a vector, and use the carry state to set the top bit.
  LogicVRegister& Halve(VectorFormat vform) {
    for (int i = 0; i < LaneCountFromFormat(vform); i++) {
      int64_t val = Int(vform, i);
      SetRounding(i, (val & 1) == 1);
      val >>= 1;
      if (GetSignedSaturation(i) != kNotSaturated) {
        // If the operation causes signed saturation, the sign bit must be
        // inverted.
        val ^= (MaxUintFromFormat(vform) >> 1) + 1;
      }
      SetInt(vform, i, val);
    }
    return *this;
  }

 private:
  SimVRegister& register_;

  // Allocate one saturation state entry per lane; largest register is type Q,
  // and lanes can be a minimum of one byte wide.
  Saturation saturated_[kQRegSizeInBytes];

  // Allocate one rounding state entry per lane.
  bool round_[kQRegSizeInBytes];
};

// The proper way to initialize a simulated system register (such as NZCV) is as
// follows:
//  SimSystemRegister nzcv = SimSystemRegister::DefaultValueFor(NZCV);
class SimSystemRegister {
 public:
  // The default constructor represents a register which has no writable bits.
  // It is not possible to set its value to anything other than 0.
  SimSystemRegister() : value_(0), write_ignore_mask_(0xffffffff) {}

  uint32_t GetRawValue() const { return value_; }
  VIXL_DEPRECATED("GetRawValue", uint32_t RawValue() const) {
    return GetRawValue();
  }

  void SetRawValue(uint32_t new_value) {
    value_ = (value_ & write_ignore_mask_) | (new_value & ~write_ignore_mask_);
  }

  uint32_t ExtractBits(int msb, int lsb) const {
    return ExtractUnsignedBitfield32(msb, lsb, value_);
  }
  VIXL_DEPRECATED("ExtractBits", uint32_t Bits(int msb, int lsb) const) {
    return ExtractBits(msb, lsb);
  }

  int32_t ExtractSignedBits(int msb, int lsb) const {
    return ExtractSignedBitfield32(msb, lsb, value_);
  }
  VIXL_DEPRECATED("ExtractSignedBits",
                  int32_t SignedBits(int msb, int lsb) const) {
    return ExtractSignedBits(msb, lsb);
  }

  void SetBits(int msb, int lsb, uint32_t bits);

  // Default system register values.
  static SimSystemRegister DefaultValueFor(SystemRegister id);

#define DEFINE_GETTER(Name, HighBit, LowBit, Func)                            \
  uint32_t Get##Name() const { return this->Func(HighBit, LowBit); }          \
  VIXL_DEPRECATED("Get" #Name, uint32_t Name() const) { return Get##Name(); } \
  void Set##Name(uint32_t bits) { SetBits(HighBit, LowBit, bits); }
#define DEFINE_WRITE_IGNORE_MASK(Name, Mask) \
  static const uint32_t Name##WriteIgnoreMask = ~static_cast<uint32_t>(Mask);

  SYSTEM_REGISTER_FIELDS_LIST(DEFINE_GETTER, DEFINE_WRITE_IGNORE_MASK)

#undef DEFINE_ZERO_BITS
#undef DEFINE_GETTER

 protected:
  // Most system registers only implement a few of the bits in the word. Other
  // bits are "read-as-zero, write-ignored". The write_ignore_mask argument
  // describes the bits which are not modifiable.
  SimSystemRegister(uint32_t value, uint32_t write_ignore_mask)
      : value_(value), write_ignore_mask_(write_ignore_mask) {}

  uint32_t value_;
  uint32_t write_ignore_mask_;
};


class SimExclusiveLocalMonitor {
 public:
  SimExclusiveLocalMonitor() : kSkipClearProbability(8), seed_(0x87654321) {
    Clear();
  }

  // Clear the exclusive monitor (like clrex).
  void Clear() {
    address_ = 0;
    size_ = 0;
  }

  // Clear the exclusive monitor most of the time.
  void MaybeClear() {
    if ((seed_ % kSkipClearProbability) != 0) {
      Clear();
    }

    // Advance seed_ using a simple linear congruential generator.
    seed_ = (seed_ * 48271) % 2147483647;
  }

  // Mark the address range for exclusive access (like load-exclusive).
  void MarkExclusive(uint64_t address, size_t size) {
    address_ = address;
    size_ = size;
  }

  // Return true if the address range is marked (like store-exclusive).
  // This helper doesn't implicitly clear the monitor.
  bool IsExclusive(uint64_t address, size_t size) {
    VIXL_ASSERT(size > 0);
    // Be pedantic: Require both the address and the size to match.
    return (size == size_) && (address == address_);
  }

 private:
  uint64_t address_;
  size_t size_;

  const int kSkipClearProbability;
  uint32_t seed_;
};


// We can't accurate simulate the global monitor since it depends on external
// influences. Instead, this implementation occasionally causes accesses to
// fail, according to kPassProbability.
class SimExclusiveGlobalMonitor {
 public:
  SimExclusiveGlobalMonitor() : kPassProbability(8), seed_(0x87654321) {}

  bool IsExclusive(uint64_t address, size_t size) {
    USE(address, size);

    bool pass = (seed_ % kPassProbability) != 0;
    // Advance seed_ using a simple linear congruential generator.
    seed_ = (seed_ * 48271) % 2147483647;
    return pass;
  }

 private:
  const int kPassProbability;
  uint32_t seed_;
};


class Simulator : public DecoderVisitor {
 public:
  explicit Simulator(Decoder* decoder, FILE* stream = stdout);
  ~Simulator();

  void ResetState();

  // Run the simulator.
  virtual void Run();
  void RunFrom(const Instruction* first);


#if defined(VIXL_HAS_ABI_SUPPORT) && __cplusplus >= 201103L && \
    (defined(__clang__) || GCC_VERSION_OR_NEWER(4, 9, 1))
  // Templated `RunFrom` version taking care of passing arguments and returning
  // the result value.
  // This allows code like:
  //    int32_t res = simulator.RunFrom<int32_t, int32_t>(GenerateCode(),
  //                                                      0x123);
  // It requires VIXL's ABI features, and C++11 or greater.
  // Also, the initialisation of tuples is incorrect in GCC before 4.9.1:
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=51253
  template <typename R, typename... P>
  R RunFrom(const Instruction* code, P... arguments) {
    return RunFromStructHelper<R, P...>::Wrapper(this, code, arguments...);
  }

  template <typename R, typename... P>
  struct RunFromStructHelper {
    static R Wrapper(Simulator* simulator,
                     const Instruction* code,
                     P... arguments) {
      ABI abi;
      std::tuple<P...> unused_tuple{
          // TODO: We currently do not support arguments passed on the stack. We
          // could do so by using `WriteGenericOperand()` here, but may need to
          // add features to handle situations where the stack is or is not set
          // up.
          (simulator->WriteCPURegister(abi.GetNextParameterGenericOperand<P>()
                                           .GetCPURegister(),
                                       arguments),
           arguments)...};
      simulator->RunFrom(code);
      return simulator->ReadGenericOperand<R>(abi.GetReturnGenericOperand<R>());
    }
  };

  // Partial specialization when the return type is `void`.
  template <typename... P>
  struct RunFromStructHelper<void, P...> {
    static void Wrapper(Simulator* simulator,
                        const Instruction* code,
                        P... arguments) {
      ABI abi;
      std::tuple<P...> unused_tuple{
          // TODO: We currently do not support arguments passed on the stack. We
          // could do so by using `WriteGenericOperand()` here, but may need to
          // add features to handle situations where the stack is or is not set
          // up.
          (simulator->WriteCPURegister(abi.GetNextParameterGenericOperand<P>()
                                           .GetCPURegister(),
                                       arguments),
           arguments)...};
      simulator->RunFrom(code);
    }
  };
#endif

  // Execution ends when the PC hits this address.
  static const Instruction* kEndOfSimAddress;

  // Simulation helpers.
  const Instruction* ReadPc() const { return pc_; }
  VIXL_DEPRECATED("ReadPc", const Instruction* pc() const) { return ReadPc(); }

  enum BranchLogMode { LogBranches, NoBranchLog };

  void WritePc(const Instruction* new_pc,
               BranchLogMode log_mode = LogBranches) {
    if (log_mode == LogBranches) LogTakenBranch(new_pc);
    pc_ = Memory::AddressUntag(new_pc);
    pc_modified_ = true;
  }
  VIXL_DEPRECATED("WritePc", void set_pc(const Instruction* new_pc)) {
    return WritePc(new_pc);
  }

  void IncrementPc() {
    if (!pc_modified_) {
      pc_ = pc_->GetNextInstruction();
    }
  }
  VIXL_DEPRECATED("IncrementPc", void increment_pc()) { IncrementPc(); }

  void ExecuteInstruction() {
    // The program counter should always be aligned.
    VIXL_ASSERT(IsWordAligned(pc_));
    pc_modified_ = false;

    // decoder_->Decode(...) triggers at least the following visitors:
    //  1. The CPUFeaturesAuditor (`cpu_features_auditor_`).
    //  2. The PrintDisassembler (`print_disasm_`), if enabled.
    //  3. The Simulator (`this`).
    // User can add additional visitors at any point, but the Simulator requires
    // that the ordering above is preserved.
    decoder_->Decode(pc_);
    IncrementPc();
    LogAllWrittenRegisters();

    VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable());
  }

// Declare all Visitor functions.
#define DECLARE(A) \
  virtual void Visit##A(const Instruction* instr) VIXL_OVERRIDE;
  VISITOR_LIST_THAT_RETURN(DECLARE)
#undef DECLARE

#define DECLARE(A)                                                     \
  VIXL_DEBUG_NO_RETURN virtual void Visit##A(const Instruction* instr) \
      VIXL_OVERRIDE;
  VISITOR_LIST_THAT_DONT_RETURN(DECLARE)
#undef DECLARE


  // Integer register accessors.

  // Basic accessor: Read the register as the specified type.
  template <typename T>
  T ReadRegister(unsigned code, Reg31Mode r31mode = Reg31IsZeroRegister) const {
    VIXL_ASSERT(
        code < kNumberOfRegisters ||
        ((r31mode == Reg31IsZeroRegister) && (code == kSPRegInternalCode)));
    if ((code == 31) && (r31mode == Reg31IsZeroRegister)) {
      T result;
      memset(&result, 0, sizeof(result));
      return result;
    }
    if ((r31mode == Reg31IsZeroRegister) && (code == kSPRegInternalCode)) {
      code = 31;
    }
    return registers_[code].Get<T>();
  }
  template <typename T>
  VIXL_DEPRECATED("ReadRegister",
                  T reg(unsigned code, Reg31Mode r31mode = Reg31IsZeroRegister)
                      const) {
    return ReadRegister<T>(code, r31mode);
  }

  // Common specialized accessors for the ReadRegister() template.
  int32_t ReadWRegister(unsigned code,
                        Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return ReadRegister<int32_t>(code, r31mode);
  }
  VIXL_DEPRECATED("ReadWRegister",
                  int32_t wreg(unsigned code,
                               Reg31Mode r31mode = Reg31IsZeroRegister) const) {
    return ReadWRegister(code, r31mode);
  }

  int64_t ReadXRegister(unsigned code,
                        Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return ReadRegister<int64_t>(code, r31mode);
  }
  VIXL_DEPRECATED("ReadXRegister",
                  int64_t xreg(unsigned code,
                               Reg31Mode r31mode = Reg31IsZeroRegister) const) {
    return ReadXRegister(code, r31mode);
  }

  // As above, with parameterized size and return type. The value is
  // either zero-extended or truncated to fit, as required.
  template <typename T>
  T ReadRegister(unsigned size,
                 unsigned code,
                 Reg31Mode r31mode = Reg31IsZeroRegister) const {
    uint64_t raw;
    switch (size) {
      case kWRegSize:
        raw = ReadRegister<uint32_t>(code, r31mode);
        break;
      case kXRegSize:
        raw = ReadRegister<uint64_t>(code, r31mode);
        break;
      default:
        VIXL_UNREACHABLE();
        return 0;
    }

    T result;
    VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(raw));
    // Copy the result and truncate to fit. This assumes a little-endian host.
    memcpy(&result, &raw, sizeof(result));
    return result;
  }
  template <typename T>
  VIXL_DEPRECATED("ReadRegister",
                  T reg(unsigned size,
                        unsigned code,
                        Reg31Mode r31mode = Reg31IsZeroRegister) const) {
    return ReadRegister<T>(size, code, r31mode);
  }

  // Use int64_t by default if T is not specified.
  int64_t ReadRegister(unsigned size,
                       unsigned code,
                       Reg31Mode r31mode = Reg31IsZeroRegister) const {
    return ReadRegister<int64_t>(size, code, r31mode);
  }
  VIXL_DEPRECATED("ReadRegister",
                  int64_t reg(unsigned size,
                              unsigned code,
                              Reg31Mode r31mode = Reg31IsZeroRegister) const) {
    return ReadRegister(size, code, r31mode);
  }

  enum RegLogMode { LogRegWrites, NoRegLog };

  // Write 'value' into an integer register. The value is zero-extended. This
  // behaviour matches AArch64 register writes.
  template <typename T>
  void WriteRegister(unsigned code,
                     T value,
                     RegLogMode log_mode = LogRegWrites,
                     Reg31Mode r31mode = Reg31IsZeroRegister) {
    if (sizeof(T) < kWRegSizeInBytes) {
      // We use a C-style cast on purpose here.
      // Since we do not have access to 'constepxr if', the casts in this `if`
      // must be valid even if we know the code will never be executed, in
      // particular when `T` is a pointer type.
      int64_t tmp_64bit = (int64_t)value;
      int32_t tmp_32bit = static_cast<int32_t>(tmp_64bit);
      WriteRegister<int32_t>(code, tmp_32bit, log_mode, r31mode);
      return;
    }

    VIXL_ASSERT((sizeof(T) == kWRegSizeInBytes) ||
                (sizeof(T) == kXRegSizeInBytes));
    VIXL_ASSERT(
        code < kNumberOfRegisters ||
        ((r31mode == Reg31IsZeroRegister) && (code == kSPRegInternalCode)));

    if ((code == 31) && (r31mode == Reg31IsZeroRegister)) {
      return;
    }

    if ((r31mode == Reg31IsZeroRegister) && (code == kSPRegInternalCode)) {
      code = 31;
    }

    registers_[code].Write(value);

    if (log_mode == LogRegWrites) LogRegister(code, r31mode);
  }
  template <typename T>
  VIXL_DEPRECATED("WriteRegister",
                  void set_reg(unsigned code,
                               T value,
                               RegLogMode log_mode = LogRegWrites,
                               Reg31Mode r31mode = Reg31IsZeroRegister)) {
    WriteRegister<T>(code, value, log_mode, r31mode);
  }

  // Common specialized accessors for the set_reg() template.
  void WriteWRegister(unsigned code,
                      int32_t value,
                      RegLogMode log_mode = LogRegWrites,
                      Reg31Mode r31mode = Reg31IsZeroRegister) {
    WriteRegister(code, value, log_mode, r31mode);
  }
  VIXL_DEPRECATED("WriteWRegister",
                  void set_wreg(unsigned code,
                                int32_t value,
                                RegLogMode log_mode = LogRegWrites,
                                Reg31Mode r31mode = Reg31IsZeroRegister)) {
    WriteWRegister(code, value, log_mode, r31mode);
  }

  void WriteXRegister(unsigned code,
                      int64_t value,
                      RegLogMode log_mode = LogRegWrites,
                      Reg31Mode r31mode = Reg31IsZeroRegister) {
    WriteRegister(code, value, log_mode, r31mode);
  }
  VIXL_DEPRECATED("WriteXRegister",
                  void set_xreg(unsigned code,
                                int64_t value,
                                RegLogMode log_mode = LogRegWrites,
                                Reg31Mode r31mode = Reg31IsZeroRegister)) {
    WriteXRegister(code, value, log_mode, r31mode);
  }

  // As above, with parameterized size and type. The value is either
  // zero-extended or truncated to fit, as required.
  template <typename T>
  void WriteRegister(unsigned size,
                     unsigned code,
                     T value,
                     RegLogMode log_mode = LogRegWrites,
                     Reg31Mode r31mode = Reg31IsZeroRegister) {
    // Zero-extend the input.
    uint64_t raw = 0;
    VIXL_STATIC_ASSERT(sizeof(value) <= sizeof(raw));
    memcpy(&raw, &value, sizeof(value));

    // Write (and possibly truncate) the value.
    switch (size) {
      case kWRegSize:
        WriteRegister(code, static_cast<uint32_t>(raw), log_mode, r31mode);
        break;
      case kXRegSize:
        WriteRegister(code, raw, log_mode, r31mode);
        break;
      default:
        VIXL_UNREACHABLE();
        return;
    }
  }
  template <typename T>
  VIXL_DEPRECATED("WriteRegister",
                  void set_reg(unsigned size,
                               unsigned code,
                               T value,
                               RegLogMode log_mode = LogRegWrites,
                               Reg31Mode r31mode = Reg31IsZeroRegister)) {
    WriteRegister(size, code, value, log_mode, r31mode);
  }

  // Common specialized accessors for the set_reg() template.

  // Commonly-used special cases.
  template <typename T>
  void WriteLr(T value) {
    WriteRegister(kLinkRegCode, value);
  }
  template <typename T>
  VIXL_DEPRECATED("WriteLr", void set_lr(T value)) {
    WriteLr(value);
  }

  template <typename T>
  void WriteSp(T value) {
    WriteRegister(31, value, LogRegWrites, Reg31IsStackPointer);
  }
  template <typename T>
  VIXL_DEPRECATED("WriteSp", void set_sp(T value)) {
    WriteSp(value);
  }

  // Vector register accessors.
  // These are equivalent to the integer register accessors, but for vector
  // registers.

  // A structure for representing a 128-bit Q register.
  struct qreg_t {
    uint8_t val[kQRegSizeInBytes];
  };

  // Basic accessor: read the register as the specified type.
  template <typename T>
  T ReadVRegister(unsigned code) const {
    VIXL_STATIC_ASSERT(
        (sizeof(T) == kBRegSizeInBytes) || (sizeof(T) == kHRegSizeInBytes) ||
        (sizeof(T) == kSRegSizeInBytes) || (sizeof(T) == kDRegSizeInBytes) ||
        (sizeof(T) == kQRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfVRegisters);

    return vregisters_[code].Get<T>();
  }
  template <typename T>
  VIXL_DEPRECATED("ReadVRegister", T vreg(unsigned code) const) {
    return ReadVRegister<T>(code);
  }

  // Common specialized accessors for the vreg() template.
  int8_t ReadBRegister(unsigned code) const {
    return ReadVRegister<int8_t>(code);
  }
  VIXL_DEPRECATED("ReadBRegister", int8_t breg(unsigned code) const) {
    return ReadBRegister(code);
  }

  vixl::internal::SimFloat16 ReadHRegister(unsigned code) const {
    return RawbitsToFloat16(ReadHRegisterBits(code));
  }
  VIXL_DEPRECATED("ReadHRegister", int16_t hreg(unsigned code) const) {
    return Float16ToRawbits(ReadHRegister(code));
  }

  uint16_t ReadHRegisterBits(unsigned code) const {
    return ReadVRegister<uint16_t>(code);
  }

  float ReadSRegister(unsigned code) const {
    return ReadVRegister<float>(code);
  }
  VIXL_DEPRECATED("ReadSRegister", float sreg(unsigned code) const) {
    return ReadSRegister(code);
  }

  uint32_t ReadSRegisterBits(unsigned code) const {
    return ReadVRegister<uint32_t>(code);
  }
  VIXL_DEPRECATED("ReadSRegisterBits",
                  uint32_t sreg_bits(unsigned code) const) {
    return ReadSRegisterBits(code);
  }

  double ReadDRegister(unsigned code) const {
    return ReadVRegister<double>(code);
  }
  VIXL_DEPRECATED("ReadDRegister", double dreg(unsigned code) const) {
    return ReadDRegister(code);
  }

  uint64_t ReadDRegisterBits(unsigned code) const {
    return ReadVRegister<uint64_t>(code);
  }
  VIXL_DEPRECATED("ReadDRegisterBits",
                  uint64_t dreg_bits(unsigned code) const) {
    return ReadDRegisterBits(code);
  }

  qreg_t ReadQRegister(unsigned code) const {
    return ReadVRegister<qreg_t>(code);
  }
  VIXL_DEPRECATED("ReadQRegister", qreg_t qreg(unsigned code) const) {
    return ReadQRegister(code);
  }

  // As above, with parameterized size and return type. The value is
  // either zero-extended or truncated to fit, as required.
  template <typename T>
  T ReadVRegister(unsigned size, unsigned code) const {
    uint64_t raw = 0;
    T result;

    switch (size) {
      case kSRegSize:
        raw = ReadVRegister<uint32_t>(code);
        break;
      case kDRegSize:
        raw = ReadVRegister<uint64_t>(code);
        break;
      default:
        VIXL_UNREACHABLE();
        break;
    }

    VIXL_STATIC_ASSERT(sizeof(result) <= sizeof(raw));
    // Copy the result and truncate to fit. This assumes a little-endian host.
    memcpy(&result, &raw, sizeof(result));
    return result;
  }
  template <typename T>
  VIXL_DEPRECATED("ReadVRegister", T vreg(unsigned size, unsigned code) const) {
    return ReadVRegister<T>(size, code);
  }

  SimVRegister& ReadVRegister(unsigned code) { return vregisters_[code]; }
  VIXL_DEPRECATED("ReadVRegister", SimVRegister& vreg(unsigned code)) {
    return ReadVRegister(code);
  }

  // Basic accessor: Write the specified value.
  template <typename T>
  void WriteVRegister(unsigned code,
                      T value,
                      RegLogMode log_mode = LogRegWrites) {
    VIXL_STATIC_ASSERT((sizeof(value) == kBRegSizeInBytes) ||
                       (sizeof(value) == kHRegSizeInBytes) ||
                       (sizeof(value) == kSRegSizeInBytes) ||
                       (sizeof(value) == kDRegSizeInBytes) ||
                       (sizeof(value) == kQRegSizeInBytes));
    VIXL_ASSERT(code < kNumberOfVRegisters);
    vregisters_[code].Write(value);

    if (log_mode == LogRegWrites) {
      LogVRegister(code, GetPrintRegisterFormat(value));
    }
  }
  template <typename T>
  VIXL_DEPRECATED("WriteVRegister",
                  void set_vreg(unsigned code,
                                T value,
                                RegLogMode log_mode = LogRegWrites)) {
    WriteVRegister(code, value, log_mode);
  }

  // Common specialized accessors for the WriteVRegister() template.
  void WriteBRegister(unsigned code,
                      int8_t value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteBRegister",
                  void set_breg(unsigned code,
                                int8_t value,
                                RegLogMode log_mode = LogRegWrites)) {
    return WriteBRegister(code, value, log_mode);
  }

  void WriteHRegister(unsigned code,
                      vixl::internal::SimFloat16 value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, Float16ToRawbits(value), log_mode);
  }

  void WriteHRegister(unsigned code,
                      int16_t value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteHRegister",
                  void set_hreg(unsigned code,
                                int16_t value,
                                RegLogMode log_mode = LogRegWrites)) {
    return WriteHRegister(code, value, log_mode);
  }

  void WriteSRegister(unsigned code,
                      float value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteSRegister",
                  void set_sreg(unsigned code,
                                float value,
                                RegLogMode log_mode = LogRegWrites)) {
    WriteSRegister(code, value, log_mode);
  }

  void WriteSRegisterBits(unsigned code,
                          uint32_t value,
                          RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteSRegisterBits",
                  void set_sreg_bits(unsigned code,
                                     uint32_t value,
                                     RegLogMode log_mode = LogRegWrites)) {
    WriteSRegisterBits(code, value, log_mode);
  }

  void WriteDRegister(unsigned code,
                      double value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteDRegister",
                  void set_dreg(unsigned code,
                                double value,
                                RegLogMode log_mode = LogRegWrites)) {
    WriteDRegister(code, value, log_mode);
  }

  void WriteDRegisterBits(unsigned code,
                          uint64_t value,
                          RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteDRegisterBits",
                  void set_dreg_bits(unsigned code,
                                     uint64_t value,
                                     RegLogMode log_mode = LogRegWrites)) {
    WriteDRegisterBits(code, value, log_mode);
  }

  void WriteQRegister(unsigned code,
                      qreg_t value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister(code, value, log_mode);
  }
  VIXL_DEPRECATED("WriteQRegister",
                  void set_qreg(unsigned code,
                                qreg_t value,
                                RegLogMode log_mode = LogRegWrites)) {
    WriteQRegister(code, value, log_mode);
  }

  template <typename T>
  T ReadRegister(Register reg) const {
    return ReadRegister<T>(reg.GetCode(), Reg31IsZeroRegister);
  }

  template <typename T>
  void WriteRegister(Register reg,
                     T value,
                     RegLogMode log_mode = LogRegWrites) {
    WriteRegister<T>(reg.GetCode(), value, log_mode, Reg31IsZeroRegister);
  }

  template <typename T>
  T ReadVRegister(VRegister vreg) const {
    return ReadVRegister<T>(vreg.GetCode());
  }

  template <typename T>
  void WriteVRegister(VRegister vreg,
                      T value,
                      RegLogMode log_mode = LogRegWrites) {
    WriteVRegister<T>(vreg.GetCode(), value, log_mode);
  }

  template <typename T>
  T ReadCPURegister(CPURegister reg) const {
    if (reg.IsVRegister()) {
      return ReadVRegister<T>(VRegister(reg));
    } else {
      return ReadRegister<T>(Register(reg));
    }
  }

  template <typename T>
  void WriteCPURegister(CPURegister reg,
                        T value,
                        RegLogMode log_mode = LogRegWrites) {
    if (reg.IsVRegister()) {
      WriteVRegister<T>(VRegister(reg), value, log_mode);
    } else {
      WriteRegister<T>(Register(reg), value, log_mode);
    }
  }

  uint64_t ComputeMemOperandAddress(const MemOperand& mem_op) const;

  template <typename T>
  T ReadGenericOperand(GenericOperand operand) const {
    if (operand.IsCPURegister()) {
      return ReadCPURegister<T>(operand.GetCPURegister());
    } else {
      VIXL_ASSERT(operand.IsMemOperand());
      return Memory::Read<T>(ComputeMemOperandAddress(operand.GetMemOperand()));
    }
  }

  template <typename T>
  void WriteGenericOperand(GenericOperand operand,
                           T value,
                           RegLogMode log_mode = LogRegWrites) {
    if (operand.IsCPURegister()) {
      WriteCPURegister<T>(operand.GetCPURegister(), value, log_mode);
    } else {
      VIXL_ASSERT(operand.IsMemOperand());
      Memory::Write(ComputeMemOperandAddress(operand.GetMemOperand()), value);
    }
  }

  bool ReadN() const { return nzcv_.GetN() != 0; }
  VIXL_DEPRECATED("ReadN", bool N() const) { return ReadN(); }

  bool ReadZ() const { return nzcv_.GetZ() != 0; }
  VIXL_DEPRECATED("ReadZ", bool Z() const) { return ReadZ(); }

  bool ReadC() const { return nzcv_.GetC() != 0; }
  VIXL_DEPRECATED("ReadC", bool C() const) { return ReadC(); }

  bool ReadV() const { return nzcv_.GetV() != 0; }
  VIXL_DEPRECATED("ReadV", bool V() const) { return ReadV(); }

  SimSystemRegister& ReadNzcv() { return nzcv_; }
  VIXL_DEPRECATED("ReadNzcv", SimSystemRegister& nzcv()) { return ReadNzcv(); }

  // TODO: Find a way to make the fpcr_ members return the proper types, so
  // these accessors are not necessary.
  FPRounding ReadRMode() const {
    return static_cast<FPRounding>(fpcr_.GetRMode());
  }
  VIXL_DEPRECATED("ReadRMode", FPRounding RMode()) { return ReadRMode(); }

  UseDefaultNaN ReadDN() const {
    return fpcr_.GetDN() != 0 ? kUseDefaultNaN : kIgnoreDefaultNaN;
  }

  VIXL_DEPRECATED("ReadDN", bool DN()) {
    return ReadDN() == kUseDefaultNaN ? true : false;
  }

  SimSystemRegister& ReadFpcr() { return fpcr_; }
  VIXL_DEPRECATED("ReadFpcr", SimSystemRegister& fpcr()) { return ReadFpcr(); }

  // Specify relevant register formats for Print(V)Register and related helpers.
  enum PrintRegisterFormat {
    // The lane size.
    kPrintRegLaneSizeB = 0 << 0,
    kPrintRegLaneSizeH = 1 << 0,
    kPrintRegLaneSizeS = 2 << 0,
    kPrintRegLaneSizeW = kPrintRegLaneSizeS,
    kPrintRegLaneSizeD = 3 << 0,
    kPrintRegLaneSizeX = kPrintRegLaneSizeD,
    kPrintRegLaneSizeQ = 4 << 0,

    kPrintRegLaneSizeOffset = 0,
    kPrintRegLaneSizeMask = 7 << 0,

    // The lane count.
    kPrintRegAsScalar = 0,
    kPrintRegAsDVector = 1 << 3,
    kPrintRegAsQVector = 2 << 3,

    kPrintRegAsVectorMask = 3 << 3,

    // Indicate floating-point format lanes. (This flag is only supported for
    // S-, H-, and D-sized lanes.)
    kPrintRegAsFP = 1 << 5,

    // Supported combinations.

    kPrintXReg = kPrintRegLaneSizeX | kPrintRegAsScalar,
    kPrintWReg = kPrintRegLaneSizeW | kPrintRegAsScalar,
    kPrintHReg = kPrintRegLaneSizeH | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintSReg = kPrintRegLaneSizeS | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintDReg = kPrintRegLaneSizeD | kPrintRegAsScalar | kPrintRegAsFP,

    kPrintReg1B = kPrintRegLaneSizeB | kPrintRegAsScalar,
    kPrintReg8B = kPrintRegLaneSizeB | kPrintRegAsDVector,
    kPrintReg16B = kPrintRegLaneSizeB | kPrintRegAsQVector,
    kPrintReg1H = kPrintRegLaneSizeH | kPrintRegAsScalar,
    kPrintReg4H = kPrintRegLaneSizeH | kPrintRegAsDVector,
    kPrintReg8H = kPrintRegLaneSizeH | kPrintRegAsQVector,
    kPrintReg1S = kPrintRegLaneSizeS | kPrintRegAsScalar,
    kPrintReg2S = kPrintRegLaneSizeS | kPrintRegAsDVector,
    kPrintReg4S = kPrintRegLaneSizeS | kPrintRegAsQVector,
    kPrintReg1HFP = kPrintRegLaneSizeH | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg4HFP = kPrintRegLaneSizeH | kPrintRegAsDVector | kPrintRegAsFP,
    kPrintReg8HFP = kPrintRegLaneSizeH | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1SFP = kPrintRegLaneSizeS | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg2SFP = kPrintRegLaneSizeS | kPrintRegAsDVector | kPrintRegAsFP,
    kPrintReg4SFP = kPrintRegLaneSizeS | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1D = kPrintRegLaneSizeD | kPrintRegAsScalar,
    kPrintReg2D = kPrintRegLaneSizeD | kPrintRegAsQVector,
    kPrintReg1DFP = kPrintRegLaneSizeD | kPrintRegAsScalar | kPrintRegAsFP,
    kPrintReg2DFP = kPrintRegLaneSizeD | kPrintRegAsQVector | kPrintRegAsFP,
    kPrintReg1Q = kPrintRegLaneSizeQ | kPrintRegAsScalar
  };

  unsigned GetPrintRegLaneSizeInBytesLog2(PrintRegisterFormat format) {
    return (format & kPrintRegLaneSizeMask) >> kPrintRegLaneSizeOffset;
  }

  unsigned GetPrintRegLaneSizeInBytes(PrintRegisterFormat format) {
    return 1 << GetPrintRegLaneSizeInBytesLog2(format);
  }

  unsigned GetPrintRegSizeInBytesLog2(PrintRegisterFormat format) {
    if (format & kPrintRegAsDVector) return kDRegSizeInBytesLog2;
    if (format & kPrintRegAsQVector) return kQRegSizeInBytesLog2;

    // Scalar types.
    return GetPrintRegLaneSizeInBytesLog2(format);
  }

  unsigned GetPrintRegSizeInBytes(PrintRegisterFormat format) {
    return 1 << GetPrintRegSizeInBytesLog2(format);
  }

  unsigned GetPrintRegLaneCount(PrintRegisterFormat format) {
    unsigned reg_size_log2 = GetPrintRegSizeInBytesLog2(format);
    unsigned lane_size_log2 = GetPrintRegLaneSizeInBytesLog2(format);
    VIXL_ASSERT(reg_size_log2 >= lane_size_log2);
    return 1 << (reg_size_log2 - lane_size_log2);
  }

  PrintRegisterFormat GetPrintRegisterFormatForSize(unsigned reg_size,
                                                    unsigned lane_size);

  PrintRegisterFormat GetPrintRegisterFormatForSize(unsigned size) {
    return GetPrintRegisterFormatForSize(size, size);
  }

  PrintRegisterFormat GetPrintRegisterFormatForSizeFP(unsigned size) {
    switch (size) {
      default:
        VIXL_UNREACHABLE();
        return kPrintDReg;
      case kDRegSizeInBytes:
        return kPrintDReg;
      case kSRegSizeInBytes:
        return kPrintSReg;
      case kHRegSizeInBytes:
        return kPrintHReg;
    }
  }

  PrintRegisterFormat GetPrintRegisterFormatTryFP(PrintRegisterFormat format) {
    if ((GetPrintRegLaneSizeInBytes(format) == kHRegSizeInBytes) ||
        (GetPrintRegLaneSizeInBytes(format) == kSRegSizeInBytes) ||
        (GetPrintRegLaneSizeInBytes(format) == kDRegSizeInBytes)) {
      return static_cast<PrintRegisterFormat>(format | kPrintRegAsFP);
    }
    return format;
  }

  template <typename T>
  PrintRegisterFormat GetPrintRegisterFormat(T value) {
    return GetPrintRegisterFormatForSize(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(double value) {
    VIXL_STATIC_ASSERT(sizeof(value) == kDRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(float value) {
    VIXL_STATIC_ASSERT(sizeof(value) == kSRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(value));
  }

  PrintRegisterFormat GetPrintRegisterFormat(Float16 value) {
    VIXL_STATIC_ASSERT(sizeof(Float16ToRawbits(value)) == kHRegSizeInBytes);
    return GetPrintRegisterFormatForSizeFP(sizeof(Float16ToRawbits(value)));
  }

  PrintRegisterFormat GetPrintRegisterFormat(VectorFormat vform);
  PrintRegisterFormat GetPrintRegisterFormatFP(VectorFormat vform);

  // Print all registers of the specified types.
  void PrintRegisters();
  void PrintVRegisters();
  void PrintSystemRegisters();

  // As above, but only print the registers that have been updated.
  void PrintWrittenRegisters();
  void PrintWrittenVRegisters();

  // As above, but respect LOG_REG and LOG_VREG.
  void LogWrittenRegisters() {
    if (GetTraceParameters() & LOG_REGS) PrintWrittenRegisters();
  }
  void LogWrittenVRegisters() {
    if (GetTraceParameters() & LOG_VREGS) PrintWrittenVRegisters();
  }
  void LogAllWrittenRegisters() {
    LogWrittenRegisters();
    LogWrittenVRegisters();
  }

  // Print individual register values (after update).
  void PrintRegister(unsigned code, Reg31Mode r31mode = Reg31IsStackPointer);
  void PrintVRegister(unsigned code, PrintRegisterFormat format);
  void PrintSystemRegister(SystemRegister id);
  void PrintTakenBranch(const Instruction* target);

  // Like Print* (above), but respect GetTraceParameters().
  void LogRegister(unsigned code, Reg31Mode r31mode = Reg31IsStackPointer) {
    if (GetTraceParameters() & LOG_REGS) PrintRegister(code, r31mode);
  }
  void LogVRegister(unsigned code, PrintRegisterFormat format) {
    if (GetTraceParameters() & LOG_VREGS) PrintVRegister(code, format);
  }
  void LogSystemRegister(SystemRegister id) {
    if (GetTraceParameters() & LOG_SYSREGS) PrintSystemRegister(id);
  }
  void LogTakenBranch(const Instruction* target) {
    if (GetTraceParameters() & LOG_BRANCH) PrintTakenBranch(target);
  }

  // Print memory accesses.
  void PrintRead(uintptr_t address,
                 unsigned reg_code,
                 PrintRegisterFormat format);
  void PrintWrite(uintptr_t address,
                  unsigned reg_code,
                  PrintRegisterFormat format);
  void PrintVRead(uintptr_t address,
                  unsigned reg_code,
                  PrintRegisterFormat format,
                  unsigned lane);
  void PrintVWrite(uintptr_t address,
                   unsigned reg_code,
                   PrintRegisterFormat format,
                   unsigned lane);

  // Like Print* (above), but respect GetTraceParameters().
  void LogRead(uintptr_t address,
               unsigned reg_code,
               PrintRegisterFormat format) {
    if (GetTraceParameters() & LOG_REGS) PrintRead(address, reg_code, format);
  }
  void LogWrite(uintptr_t address,
                unsigned reg_code,
                PrintRegisterFormat format) {
    if (GetTraceParameters() & LOG_WRITE) PrintWrite(address, reg_code, format);
  }
  void LogVRead(uintptr_t address,
                unsigned reg_code,
                PrintRegisterFormat format,
                unsigned lane = 0) {
    if (GetTraceParameters() & LOG_VREGS) {
      PrintVRead(address, reg_code, format, lane);
    }
  }
  void LogVWrite(uintptr_t address,
                 unsigned reg_code,
                 PrintRegisterFormat format,
                 unsigned lane = 0) {
    if (GetTraceParameters() & LOG_WRITE) {
      PrintVWrite(address, reg_code, format, lane);
    }
  }

  // Helper functions for register tracing.
  void PrintRegisterRawHelper(unsigned code,
                              Reg31Mode r31mode,
                              int size_in_bytes = kXRegSizeInBytes);
  void PrintVRegisterRawHelper(unsigned code,
                               int bytes = kQRegSizeInBytes,
                               int lsb = 0);
  void PrintVRegisterFPHelper(unsigned code,
                              unsigned lane_size_in_bytes,
                              int lane_count = 1,
                              int rightmost_lane = 0);

  VIXL_NO_RETURN void DoUnreachable(const Instruction* instr);
  void DoTrace(const Instruction* instr);
  void DoLog(const Instruction* instr);

  static const char* WRegNameForCode(unsigned code,
                                     Reg31Mode mode = Reg31IsZeroRegister);
  static const char* XRegNameForCode(unsigned code,
                                     Reg31Mode mode = Reg31IsZeroRegister);
  static const char* HRegNameForCode(unsigned code);
  static const char* SRegNameForCode(unsigned code);
  static const char* DRegNameForCode(unsigned code);
  static const char* VRegNameForCode(unsigned code);

  bool IsColouredTrace() const { return coloured_trace_; }
  VIXL_DEPRECATED("IsColouredTrace", bool coloured_trace() const) {
    return IsColouredTrace();
  }

  void SetColouredTrace(bool value);
  VIXL_DEPRECATED("SetColouredTrace", void set_coloured_trace(bool value)) {
    SetColouredTrace(value);
  }

  // Values for traces parameters defined in simulator-constants-aarch64.h in
  // enum TraceParameters.
  int GetTraceParameters() const { return trace_parameters_; }
  VIXL_DEPRECATED("GetTraceParameters", int trace_parameters() const) {
    return GetTraceParameters();
  }

  void SetTraceParameters(int parameters);
  VIXL_DEPRECATED("SetTraceParameters",
                  void set_trace_parameters(int parameters)) {
    SetTraceParameters(parameters);
  }

  void SetInstructionStats(bool value);
  VIXL_DEPRECATED("SetInstructionStats",
                  void set_instruction_stats(bool value)) {
    SetInstructionStats(value);
  }

  // Clear the simulated local monitor to force the next store-exclusive
  // instruction to fail.
  void ClearLocalMonitor() { local_monitor_.Clear(); }

  void SilenceExclusiveAccessWarning() {
    print_exclusive_access_warning_ = false;
  }

  enum PointerType { kDataPointer, kInstructionPointer };

  struct PACKey {
    uint64_t high;
    uint64_t low;
    int number;
  };

  // Current implementation is that all pointers are tagged.
  bool HasTBI(uint64_t ptr, PointerType type) {
    USE(ptr, type);
    return true;
  }

  // Current implementation uses 48-bit virtual addresses.
  int GetBottomPACBit(uint64_t ptr, int ttbr) {
    USE(ptr, ttbr);
    VIXL_ASSERT((ttbr == 0) || (ttbr == 1));
    return 48;
  }

  // The top PAC bit is 55 for the purposes of relative bit fields with TBI,
  // however bit 55 is the TTBR bit regardless of TBI so isn't part of the PAC
  // codes in pointers.
  int GetTopPACBit(uint64_t ptr, PointerType type) {
    return HasTBI(ptr, type) ? 55 : 63;
  }

  // Armv8.3 Pointer authentication helpers.
  uint64_t CalculatePACMask(uint64_t ptr, PointerType type, int ext_bit);
  uint64_t ComputePAC(uint64_t data, uint64_t context, PACKey key);
  uint64_t AuthPAC(uint64_t ptr,
                   uint64_t context,
                   PACKey key,
                   PointerType type);
  uint64_t AddPAC(uint64_t ptr, uint64_t context, PACKey key, PointerType type);
  uint64_t StripPAC(uint64_t ptr, PointerType type);

  // The common CPUFeatures interface with the set of available features.

  CPUFeatures* GetCPUFeatures() {
    return cpu_features_auditor_.GetCPUFeatures();
  }

  void SetCPUFeatures(const CPUFeatures& cpu_features) {
    cpu_features_auditor_.SetCPUFeatures(cpu_features);
  }

  // The set of features that the simulator has encountered.
  const CPUFeatures& GetSeenFeatures() {
    return cpu_features_auditor_.GetSeenFeatures();
  }
  void ResetSeenFeatures() { cpu_features_auditor_.ResetSeenFeatures(); }

// Runtime call emulation support.
// It requires VIXL's ABI features, and C++11 or greater.
// Also, the initialisation of the tuples in RuntimeCall(Non)Void is incorrect
// in GCC before 4.9.1: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=51253
#if defined(VIXL_HAS_ABI_SUPPORT) && __cplusplus >= 201103L && \
    (defined(__clang__) || GCC_VERSION_OR_NEWER(4, 9, 1))

#define VIXL_HAS_SIMULATED_RUNTIME_CALL_SUPPORT

// The implementation of the runtime call helpers require the functionality
// provided by `std::index_sequence`. It is only available from C++14, but
// we want runtime call simulation to work from C++11, so we emulate if
// necessary.
#if __cplusplus >= 201402L
  template <std::size_t... I>
  using local_index_sequence = std::index_sequence<I...>;
  template <typename... P>
  using __local_index_sequence_for = std::index_sequence_for<P...>;
#else
  // Emulate the behaviour of `std::index_sequence` and
  // `std::index_sequence_for`.
  // Naming follow the `std` names, prefixed with `emulated_`.
  template <size_t... I>
  struct emulated_index_sequence {};

  // A recursive template to create a sequence of indexes.
  // The base case (for `N == 0`) is declared outside of the class scope, as
  // required by C++.
  template <std::size_t N, size_t... I>
  struct emulated_make_index_sequence_helper
      : emulated_make_index_sequence_helper<N - 1, N - 1, I...> {};

  template <std::size_t N>
  struct emulated_make_index_sequence : emulated_make_index_sequence_helper<N> {
  };

  template <typename... P>
  struct emulated_index_sequence_for
      : emulated_make_index_sequence<sizeof...(P)> {};

  template <std::size_t... I>
  using local_index_sequence = emulated_index_sequence<I...>;
  template <typename... P>
  using __local_index_sequence_for = emulated_index_sequence_for<P...>;
#endif

  // Expand the argument tuple and perform the call.
  template <typename R, typename... P, std::size_t... I>
  R DoRuntimeCall(R (*function)(P...),
                  std::tuple<P...> arguments,
                  local_index_sequence<I...>) {
    return function(std::get<I>(arguments)...);
  }

  template <typename R, typename... P>
  void RuntimeCallNonVoid(R (*function)(P...)) {
    ABI abi;
    std::tuple<P...> argument_operands{
        ReadGenericOperand<P>(abi.GetNextParameterGenericOperand<P>())...};
    R return_value = DoRuntimeCall(function,
                                   argument_operands,
                                   __local_index_sequence_for<P...>{});
    WriteGenericOperand(abi.GetReturnGenericOperand<R>(), return_value);
  }

  template <typename R, typename... P>
  void RuntimeCallVoid(R (*function)(P...)) {
    ABI abi;
    std::tuple<P...> argument_operands{
        ReadGenericOperand<P>(abi.GetNextParameterGenericOperand<P>())...};
    DoRuntimeCall(function,
                  argument_operands,
                  __local_index_sequence_for<P...>{});
  }

  // We use `struct` for `void` return type specialisation.
  template <typename R, typename... P>
  struct RuntimeCallStructHelper {
    static void Wrapper(Simulator* simulator, uintptr_t function_pointer) {
      R (*function)(P...) = reinterpret_cast<R (*)(P...)>(function_pointer);
      simulator->RuntimeCallNonVoid(function);
    }
  };

  // Partial specialization when the return type is `void`.
  template <typename... P>
  struct RuntimeCallStructHelper<void, P...> {
    static void Wrapper(Simulator* simulator, uintptr_t function_pointer) {
      void (*function)(P...) =
          reinterpret_cast<void (*)(P...)>(function_pointer);
      simulator->RuntimeCallVoid(function);
    }
  };
#endif

 protected:
  const char* clr_normal;
  const char* clr_flag_name;
  const char* clr_flag_value;
  const char* clr_reg_name;
  const char* clr_reg_value;
  const char* clr_vreg_name;
  const char* clr_vreg_value;
  const char* clr_memory_address;
  const char* clr_warning;
  const char* clr_warning_message;
  const char* clr_printf;
  const char* clr_branch_marker;

  // Simulation helpers ------------------------------------
  bool ConditionPassed(Condition cond) {
    switch (cond) {
      case eq:
        return ReadZ();
      case ne:
        return !ReadZ();
      case hs:
        return ReadC();
      case lo:
        return !ReadC();
      case mi:
        return ReadN();
      case pl:
        return !ReadN();
      case vs:
        return ReadV();
      case vc:
        return !ReadV();
      case hi:
        return ReadC() && !ReadZ();
      case ls:
        return !(ReadC() && !ReadZ());
      case ge:
        return ReadN() == ReadV();
      case lt:
        return ReadN() != ReadV();
      case gt:
        return !ReadZ() && (ReadN() == ReadV());
      case le:
        return !(!ReadZ() && (ReadN() == ReadV()));
      case nv:
        VIXL_FALLTHROUGH();
      case al:
        return true;
      default:
        VIXL_UNREACHABLE();
        return false;
    }
  }

  bool ConditionPassed(Instr cond) {
    return ConditionPassed(static_cast<Condition>(cond));
  }

  bool ConditionFailed(Condition cond) { return !ConditionPassed(cond); }

  void AddSubHelper(const Instruction* instr, int64_t op2);
  uint64_t AddWithCarry(unsigned reg_size,
                        bool set_flags,
                        uint64_t left,
                        uint64_t right,
                        int carry_in = 0);
  void LogicalHelper(const Instruction* instr, int64_t op2);
  void ConditionalCompareHelper(const Instruction* instr, int64_t op2);
  void LoadStoreHelper(const Instruction* instr,
                       int64_t offset,
                       AddrMode addrmode);
  void LoadStorePairHelper(const Instruction* instr, AddrMode addrmode);
  template <typename T>
  void CompareAndSwapHelper(const Instruction* instr);
  template <typename T>
  void CompareAndSwapPairHelper(const Instruction* instr);
  template <typename T>
  void AtomicMemorySimpleHelper(const Instruction* instr);
  template <typename T>
  void AtomicMemorySwapHelper(const Instruction* instr);
  template <typename T>
  void LoadAcquireRCpcHelper(const Instruction* instr);
  uintptr_t AddressModeHelper(unsigned addr_reg,
                              int64_t offset,
                              AddrMode addrmode);
  void NEONLoadStoreMultiStructHelper(const Instruction* instr,
                                      AddrMode addr_mode);
  void NEONLoadStoreSingleStructHelper(const Instruction* instr,
                                       AddrMode addr_mode);

  uint64_t AddressUntag(uint64_t address) { return address & ~kAddressTagMask; }

  template <typename T>
  T* AddressUntag(T* address) {
    uintptr_t address_raw = reinterpret_cast<uintptr_t>(address);
    return reinterpret_cast<T*>(AddressUntag(address_raw));
  }

  int64_t ShiftOperand(unsigned reg_size,
                       int64_t value,
                       Shift shift_type,
                       unsigned amount) const;
  int64_t ExtendValue(unsigned reg_width,
                      int64_t value,
                      Extend extend_type,
                      unsigned left_shift = 0) const;
  uint16_t PolynomialMult(uint8_t op1, uint8_t op2) const;

  void ld1(VectorFormat vform, LogicVRegister dst, uint64_t addr);
  void ld1(VectorFormat vform, LogicVRegister dst, int index, uint64_t addr);
  void ld1r(VectorFormat vform, LogicVRegister dst, uint64_t addr);
  void ld2(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           uint64_t addr);
  void ld2(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           int index,
           uint64_t addr);
  void ld2r(VectorFormat vform,
            LogicVRegister dst1,
            LogicVRegister dst2,
            uint64_t addr);
  void ld3(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           uint64_t addr);
  void ld3(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           int index,
           uint64_t addr);
  void ld3r(VectorFormat vform,
            LogicVRegister dst1,
            LogicVRegister dst2,
            LogicVRegister dst3,
            uint64_t addr);
  void ld4(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           uint64_t addr);
  void ld4(VectorFormat vform,
           LogicVRegister dst1,
           LogicVRegister dst2,
           LogicVRegister dst3,
           LogicVRegister dst4,
           int index,
           uint64_t addr);
  void ld4r(VectorFormat vform,
            LogicVRegister dst1,
            LogicVRegister dst2,
            LogicVRegister dst3,
            LogicVRegister dst4,
            uint64_t addr);
  void st1(VectorFormat vform, LogicVRegister src, uint64_t addr);
  void st1(VectorFormat vform, LogicVRegister src, int index, uint64_t addr);
  void st2(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           uint64_t addr);
  void st2(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           int index,
           uint64_t addr);
  void st3(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           uint64_t addr);
  void st3(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           int index,
           uint64_t addr);
  void st4(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           LogicVRegister src4,
           uint64_t addr);
  void st4(VectorFormat vform,
           LogicVRegister src,
           LogicVRegister src2,
           LogicVRegister src3,
           LogicVRegister src4,
           int index,
           uint64_t addr);
  LogicVRegister cmp(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     Condition cond);
  LogicVRegister cmp(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     int imm,
                     Condition cond);
  LogicVRegister cmptst(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister add(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister addp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister mla(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mul(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister mul(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister mla(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister mls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  LogicVRegister pmul(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);

  typedef LogicVRegister (Simulator::*ByElementOp)(VectorFormat vform,
                                                   LogicVRegister dst,
                                                   const LogicVRegister& src1,
                                                   const LogicVRegister& src2,
                                                   int index);
  LogicVRegister fmul(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister fmulx(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smull(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smull2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umull(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umull2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister smlal(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smlal2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umlal(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umlal2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister smlsl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister smlsl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister umlsl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index);
  LogicVRegister umlsl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2,
                        int index);
  LogicVRegister sqdmull(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmull2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmlal(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmlal2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmlsl(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqdmlsl2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sqdmulh(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         int index);
  LogicVRegister sqrdmulh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sdot(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister sqrdmlah(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister udot(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      int index);
  LogicVRegister sqrdmlsh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          int index);
  LogicVRegister sub(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister and_(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister orr(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister orn(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister eor(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bic(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bic(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     uint64_t imm);
  LogicVRegister bif(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bit(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister bsl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2);
  LogicVRegister cls(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister clz(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister cnt(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister not_(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister rbit(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister rev(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int revSize);
  LogicVRegister rev16(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister rev32(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister rev64(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister addlp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       bool is_signed,
                       bool do_accumulate);
  LogicVRegister saddlp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister uaddlp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sadalp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister uadalp(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister ext(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     int index);
  template <typename T>
  LogicVRegister fcadd(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int rot);
  LogicVRegister fcadd(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int rot);
  template <typename T>
  LogicVRegister fcmla(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index,
                       int rot);
  LogicVRegister fcmla(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int index,
                       int rot);
  template <typename T>
  LogicVRegister fcmla(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int rot);
  LogicVRegister fcmla(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2,
                       int rot);
  LogicVRegister ins_element(VectorFormat vform,
                             LogicVRegister dst,
                             int dst_index,
                             const LogicVRegister& src,
                             int src_index);
  LogicVRegister ins_immediate(VectorFormat vform,
                               LogicVRegister dst,
                               int dst_index,
                               uint64_t imm);
  LogicVRegister dup_element(VectorFormat vform,
                             LogicVRegister dst,
                             const LogicVRegister& src,
                             int src_index);
  LogicVRegister dup_immediate(VectorFormat vform,
                               LogicVRegister dst,
                               uint64_t imm);
  LogicVRegister movi(VectorFormat vform, LogicVRegister dst, uint64_t imm);
  LogicVRegister mvni(VectorFormat vform, LogicVRegister dst, uint64_t imm);
  LogicVRegister orr(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     uint64_t imm);
  LogicVRegister sshl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister ushl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister sminmax(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool max);
  LogicVRegister smax(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister smin(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister sminmaxp(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool max);
  LogicVRegister smaxp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister sminp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister addp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister addv(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister uaddlv(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister saddlv(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister smaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uxtl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister uxtl2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sxtl(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister sxtl2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& ind);
  LogicVRegister tbl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& tab4,
                     const LogicVRegister& ind);
  LogicVRegister Table(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& ind,
                       bool zero_out_of_bounds,
                       const LogicVRegister* tab1,
                       const LogicVRegister* tab2 = NULL,
                       const LogicVRegister* tab3 = NULL,
                       const LogicVRegister* tab4 = NULL);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& ind);
  LogicVRegister tbx(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& tab,
                     const LogicVRegister& tab2,
                     const LogicVRegister& tab3,
                     const LogicVRegister& tab4,
                     const LogicVRegister& ind);
  LogicVRegister uaddl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uaddl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister uaddw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uaddw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister saddl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister saddl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister saddw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister saddw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister usubl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister usubl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister usubw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister usubw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister ssubl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister ssubl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister ssubw(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister ssubw2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister uminmax(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool max);
  LogicVRegister umax(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister umin(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uminmaxp(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool max);
  LogicVRegister umaxp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uminp(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);
  LogicVRegister uminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          bool max);
  LogicVRegister umaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister trn1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister trn2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister zip1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister zip2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uzp1(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uzp2(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister shl(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister scvtf(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int fbits,
                       FPRounding rounding_mode);
  LogicVRegister ucvtf(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int fbits,
                       FPRounding rounding_mode);
  LogicVRegister sshll(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister sshll2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister shll(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister shll2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister ushll(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister ushll2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister sli(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister sri(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src,
                     int shift);
  LogicVRegister sshr(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister ushr(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister ssra(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister usra(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister srsra(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister ursra(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister suqadd(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister usqadd(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister sqshl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister uqshl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister sqshlu(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister abs(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister neg(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister extractnarrow(VectorFormat vform,
                               LogicVRegister dst,
                               bool dstIsSigned,
                               const LogicVRegister& src,
                               bool srcIsSigned);
  LogicVRegister xtn(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src);
  LogicVRegister sqxtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister uqxtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister sqxtun(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister absdiff(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         bool issigned);
  LogicVRegister saba(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister uaba(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister shrn(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src,
                      int shift);
  LogicVRegister shrn2(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister rshrn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       int shift);
  LogicVRegister rshrn2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister uqshrn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister uqshrn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister uqrshrn(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister uqrshrn2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqshrn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        int shift);
  LogicVRegister sqshrn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqrshrn(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqrshrn2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqshrun(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src,
                         int shift);
  LogicVRegister sqshrun2(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqrshrun(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          int shift);
  LogicVRegister sqrshrun2(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src,
                           int shift);
  LogicVRegister sqrdmulh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool round = true);
  LogicVRegister dot(VectorFormat vform,
                     LogicVRegister dst,
                     const LogicVRegister& src1,
                     const LogicVRegister& src2,
                     bool is_signed);
  LogicVRegister sdot(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister udot(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister sqrdmlash(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src1,
                           const LogicVRegister& src2,
                           bool round = true,
                           bool sub_op = false);
  LogicVRegister sqrdmlah(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool round = true);
  LogicVRegister sqrdmlsh(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src1,
                          const LogicVRegister& src2,
                          bool round = true);
  LogicVRegister sqdmulh(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
#define NEON_3VREG_LOGIC_LIST(V) \
  V(addhn)                       \
  V(addhn2)                      \
  V(raddhn)                      \
  V(raddhn2)                     \
  V(subhn)                       \
  V(subhn2)                      \
  V(rsubhn)                      \
  V(rsubhn2)                     \
  V(pmull)                       \
  V(pmull2)                      \
  V(sabal)                       \
  V(sabal2)                      \
  V(uabal)                       \
  V(uabal2)                      \
  V(sabdl)                       \
  V(sabdl2)                      \
  V(uabdl)                       \
  V(uabdl2)                      \
  V(smull)                       \
  V(smull2)                      \
  V(umull)                       \
  V(umull2)                      \
  V(smlal)                       \
  V(smlal2)                      \
  V(umlal)                       \
  V(umlal2)                      \
  V(smlsl)                       \
  V(smlsl2)                      \
  V(umlsl)                       \
  V(umlsl2)                      \
  V(sqdmlal)                     \
  V(sqdmlal2)                    \
  V(sqdmlsl)                     \
  V(sqdmlsl2)                    \
  V(sqdmull)                     \
  V(sqdmull2)

#define DEFINE_LOGIC_FUNC(FXN)                   \
  LogicVRegister FXN(VectorFormat vform,         \
                     LogicVRegister dst,         \
                     const LogicVRegister& src1, \
                     const LogicVRegister& src2);
  NEON_3VREG_LOGIC_LIST(DEFINE_LOGIC_FUNC)
#undef DEFINE_LOGIC_FUNC

#define NEON_FP3SAME_LIST(V) \
  V(fadd, FPAdd, false)      \
  V(fsub, FPSub, true)       \
  V(fmul, FPMul, true)       \
  V(fmulx, FPMulx, true)     \
  V(fdiv, FPDiv, true)       \
  V(fmax, FPMax, false)      \
  V(fmin, FPMin, false)      \
  V(fmaxnm, FPMaxNM, false)  \
  V(fminnm, FPMinNM, false)

#define DECLARE_NEON_FP_VECTOR_OP(FN, OP, PROCNAN) \
  template <typename T>                            \
  LogicVRegister FN(VectorFormat vform,            \
                    LogicVRegister dst,            \
                    const LogicVRegister& src1,    \
                    const LogicVRegister& src2);   \
  LogicVRegister FN(VectorFormat vform,            \
                    LogicVRegister dst,            \
                    const LogicVRegister& src1,    \
                    const LogicVRegister& src2);
  NEON_FP3SAME_LIST(DECLARE_NEON_FP_VECTOR_OP)
#undef DECLARE_NEON_FP_VECTOR_OP

#define NEON_FPPAIRWISE_LIST(V) \
  V(faddp, fadd, FPAdd)         \
  V(fmaxp, fmax, FPMax)         \
  V(fmaxnmp, fmaxnm, FPMaxNM)   \
  V(fminp, fmin, FPMin)         \
  V(fminnmp, fminnm, FPMinNM)

#define DECLARE_NEON_FP_PAIR_OP(FNP, FN, OP)      \
  LogicVRegister FNP(VectorFormat vform,          \
                     LogicVRegister dst,          \
                     const LogicVRegister& src1,  \
                     const LogicVRegister& src2); \
  LogicVRegister FNP(VectorFormat vform,          \
                     LogicVRegister dst,          \
                     const LogicVRegister& src);
  NEON_FPPAIRWISE_LIST(DECLARE_NEON_FP_PAIR_OP)
#undef DECLARE_NEON_FP_PAIR_OP

  template <typename T>
  LogicVRegister frecps(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  LogicVRegister frecps(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src1,
                        const LogicVRegister& src2);
  template <typename T>
  LogicVRegister frsqrts(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  LogicVRegister frsqrts(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2);
  template <typename T>
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fmla(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  template <typename T>
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fmls(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister fnmul(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src1,
                       const LogicVRegister& src2);

  template <typename T>
  LogicVRegister fcmp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      Condition cond);
  LogicVRegister fcmp(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2,
                      Condition cond);
  LogicVRegister fabscmp(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src1,
                         const LogicVRegister& src2,
                         Condition cond);
  LogicVRegister fcmp_zero(VectorFormat vform,
                           LogicVRegister dst,
                           const LogicVRegister& src,
                           Condition cond);

  template <typename T>
  LogicVRegister fneg(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  LogicVRegister fneg(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src);
  template <typename T>
  LogicVRegister frecpx(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister frecpx(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  template <typename T>
  LogicVRegister fabs_(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fabs_(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fabd(VectorFormat vform,
                      LogicVRegister dst,
                      const LogicVRegister& src1,
                      const LogicVRegister& src2);
  LogicVRegister frint(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       bool inexact_exception = false);
  LogicVRegister fcvts(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       int fbits = 0);
  LogicVRegister fcvtu(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src,
                       FPRounding rounding_mode,
                       int fbits = 0);
  LogicVRegister fcvtl(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fcvtl2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtn(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fcvtn2(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtxn(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);
  LogicVRegister fcvtxn2(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister fsqrt(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister frsqrte(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister frecpe(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src,
                        FPRounding rounding);
  LogicVRegister ursqrte(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister urecpe(VectorFormat vform,
                        LogicVRegister dst,
                        const LogicVRegister& src);

  template <typename T>
  struct TFPMinMaxOp {
    typedef T (Simulator::*type)(T a, T b);
  };

  template <typename T>
  LogicVRegister fminmaxv(VectorFormat vform,
                          LogicVRegister dst,
                          const LogicVRegister& src,
                          typename TFPMinMaxOp<T>::type Op);

  LogicVRegister fminv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fmaxv(VectorFormat vform,
                       LogicVRegister dst,
                       const LogicVRegister& src);
  LogicVRegister fminnmv(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);
  LogicVRegister fmaxnmv(VectorFormat vform,
                         LogicVRegister dst,
                         const LogicVRegister& src);

  static const uint32_t CRC32_POLY = 0x04C11DB7;
  static const uint32_t CRC32C_POLY = 0x1EDC6F41;
  uint32_t Poly32Mod2(unsigned n, uint64_t data, uint32_t poly);
  template <typename T>
  uint32_t Crc32Checksum(uint32_t acc, T val, uint32_t poly);
  uint32_t Crc32Checksum(uint32_t acc, uint64_t val, uint32_t poly);

  void SysOp_W(int op, int64_t val);

  template <typename T>
  T FPRecipSqrtEstimate(T op);
  template <typename T>
  T FPRecipEstimate(T op, FPRounding rounding);
  template <typename T, typename R>
  R FPToFixed(T op, int fbits, bool is_signed, FPRounding rounding);

  void FPCompare(double val0, double val1, FPTrapFlags trap);
  double FPRoundInt(double value, FPRounding round_mode);
  double recip_sqrt_estimate(double a);
  double recip_estimate(double a);
  double FPRecipSqrtEstimate(double a);
  double FPRecipEstimate(double a);
  double FixedToDouble(int64_t src, int fbits, FPRounding round_mode);
  double UFixedToDouble(uint64_t src, int fbits, FPRounding round_mode);
  float FixedToFloat(int64_t src, int fbits, FPRounding round_mode);
  float UFixedToFloat(uint64_t src, int fbits, FPRounding round_mode);
  ::vixl::internal::SimFloat16 FixedToFloat16(int64_t src,
                                              int fbits,
                                              FPRounding round_mode);
  ::vixl::internal::SimFloat16 UFixedToFloat16(uint64_t src,
                                               int fbits,
                                               FPRounding round_mode);
  int16_t FPToInt16(double value, FPRounding rmode);
  int32_t FPToInt32(double value, FPRounding rmode);
  int64_t FPToInt64(double value, FPRounding rmode);
  uint16_t FPToUInt16(double value, FPRounding rmode);
  uint32_t FPToUInt32(double value, FPRounding rmode);
  uint64_t FPToUInt64(double value, FPRounding rmode);
  int32_t FPToFixedJS(double value);

  template <typename T>
  T FPAdd(T op1, T op2);

  template <typename T>
  T FPNeg(T op);

  template <typename T>
  T FPDiv(T op1, T op2);

  template <typename T>
  T FPMax(T a, T b);

  template <typename T>
  T FPMaxNM(T a, T b);

  template <typename T>
  T FPMin(T a, T b);

  template <typename T>
  T FPMinNM(T a, T b);

  template <typename T>
  T FPMul(T op1, T op2);

  template <typename T>
  T FPMulx(T op1, T op2);

  template <typename T>
  T FPMulAdd(T a, T op1, T op2);

  template <typename T>
  T FPSqrt(T op);

  template <typename T>
  T FPSub(T op1, T op2);

  template <typename T>
  T FPRecipStepFused(T op1, T op2);

  template <typename T>
  T FPRSqrtStepFused(T op1, T op2);

  // This doesn't do anything at the moment. We'll need it if we want support
  // for cumulative exception bits or floating-point exceptions.
  void FPProcessException() {}

  bool FPProcessNaNs(const Instruction* instr);

  // Pseudo Printf instruction
  void DoPrintf(const Instruction* instr);

  // Pseudo-instructions to configure CPU features dynamically.
  void DoConfigureCPUFeatures(const Instruction* instr);

  void DoSaveCPUFeatures(const Instruction* instr);
  void DoRestoreCPUFeatures(const Instruction* instr);

// Simulate a runtime call.
#ifndef VIXL_HAS_SIMULATED_RUNTIME_CALL_SUPPORT
  VIXL_NO_RETURN_IN_DEBUG_MODE
#endif
  void DoRuntimeCall(const Instruction* instr);

  // Processor state ---------------------------------------

  // Simulated monitors for exclusive access instructions.
  SimExclusiveLocalMonitor local_monitor_;
  SimExclusiveGlobalMonitor global_monitor_;

  // Output stream.
  FILE* stream_;
  PrintDisassembler* print_disasm_;

  // Instruction statistics instrumentation.
  Instrument* instrumentation_;

  // General purpose registers. Register 31 is the stack pointer.
  SimRegister registers_[kNumberOfRegisters];

  // Vector registers
  SimVRegister vregisters_[kNumberOfVRegisters];

  // Program Status Register.
  // bits[31, 27]: Condition flags N, Z, C, and V.
  //               (Negative, Zero, Carry, Overflow)
  SimSystemRegister nzcv_;

  // Floating-Point Control Register
  SimSystemRegister fpcr_;

  // Only a subset of FPCR features are supported by the simulator. This helper
  // checks that the FPCR settings are supported.
  //
  // This is checked when floating-point instructions are executed, not when
  // FPCR is set. This allows generated code to modify FPCR for external
  // functions, or to save and restore it when entering and leaving generated
  // code.
  void AssertSupportedFPCR() {
    // No flush-to-zero support.
    VIXL_ASSERT(ReadFpcr().GetFZ() == 0);
    // Ties-to-even rounding only.
    VIXL_ASSERT(ReadFpcr().GetRMode() == FPTieEven);

    // The simulator does not support half-precision operations so
    // GetFpcr().AHP() is irrelevant, and is not checked here.
  }

  static int CalcNFlag(uint64_t result, unsigned reg_size) {
    return (result >> (reg_size - 1)) & 1;
  }

  static int CalcZFlag(uint64_t result) { return (result == 0) ? 1 : 0; }

  static const uint32_t kConditionFlagsMask = 0xf0000000;

  // Stack
  byte* stack_;
  static const int stack_protection_size_ = 256;
  // 2 KB stack.
  static const int stack_size_ = 2 * 1024 + 2 * stack_protection_size_;
  byte* stack_limit_;

  Decoder* decoder_;
  // Indicates if the pc has been modified by the instruction and should not be
  // automatically incremented.
  bool pc_modified_;
  const Instruction* pc_;

  static const char* xreg_names[];
  static const char* wreg_names[];
  static const char* hreg_names[];
  static const char* sreg_names[];
  static const char* dreg_names[];
  static const char* vreg_names[];

 private:
  static const PACKey kPACKeyIA;
  static const PACKey kPACKeyIB;
  static const PACKey kPACKeyDA;
  static const PACKey kPACKeyDB;
  static const PACKey kPACKeyGA;

  template <typename T>
  static T FPDefaultNaN();

  // Standard NaN processing.
  template <typename T>
  T FPProcessNaN(T op) {
    VIXL_ASSERT(IsNaN(op));
    if (IsSignallingNaN(op)) {
      FPProcessException();
    }
    return (ReadDN() == kUseDefaultNaN) ? FPDefaultNaN<T>() : ToQuietNaN(op);
  }

  template <typename T>
  T FPProcessNaNs(T op1, T op2) {
    if (IsSignallingNaN(op1)) {
      return FPProcessNaN(op1);
    } else if (IsSignallingNaN(op2)) {
      return FPProcessNaN(op2);
    } else if (IsNaN(op1)) {
      VIXL_ASSERT(IsQuietNaN(op1));
      return FPProcessNaN(op1);
    } else if (IsNaN(op2)) {
      VIXL_ASSERT(IsQuietNaN(op2));
      return FPProcessNaN(op2);
    } else {
      return 0.0;
    }
  }

  template <typename T>
  T FPProcessNaNs3(T op1, T op2, T op3) {
    if (IsSignallingNaN(op1)) {
      return FPProcessNaN(op1);
    } else if (IsSignallingNaN(op2)) {
      return FPProcessNaN(op2);
    } else if (IsSignallingNaN(op3)) {
      return FPProcessNaN(op3);
    } else if (IsNaN(op1)) {
      VIXL_ASSERT(IsQuietNaN(op1));
      return FPProcessNaN(op1);
    } else if (IsNaN(op2)) {
      VIXL_ASSERT(IsQuietNaN(op2));
      return FPProcessNaN(op2);
    } else if (IsNaN(op3)) {
      VIXL_ASSERT(IsQuietNaN(op3));
      return FPProcessNaN(op3);
    } else {
      return 0.0;
    }
  }

  bool coloured_trace_;

  // A set of TraceParameters flags.
  int trace_parameters_;

  // Indicates whether the instruction instrumentation is active.
  bool instruction_stats_;

  // Indicates whether the exclusive-access warning has been printed.
  bool print_exclusive_access_warning_;
  void PrintExclusiveAccessWarning();

  CPUFeaturesAuditor cpu_features_auditor_;
  std::vector<CPUFeatures> saved_cpu_features_;
};

#if defined(VIXL_HAS_SIMULATED_RUNTIME_CALL_SUPPORT) && __cplusplus < 201402L
// Base case of the recursive template used to emulate C++14
// `std::index_sequence`.
template <size_t... I>
struct Simulator::emulated_make_index_sequence_helper<0, I...>
    : Simulator::emulated_index_sequence<I...> {};
#endif

}  // namespace aarch64
}  // namespace vixl

#endif  // VIXL_INCLUDE_SIMULATOR_AARCH64

#endif  // VIXL_AARCH64_SIMULATOR_AARCH64_H_
