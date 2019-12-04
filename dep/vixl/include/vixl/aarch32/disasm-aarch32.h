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

#ifndef VIXL_DISASM_AARCH32_H_
#define VIXL_DISASM_AARCH32_H_

extern "C" {
#include <stdint.h>
}

#include <iomanip>

#include "aarch32/constants-aarch32.h"
#include "aarch32/operands-aarch32.h"

namespace vixl {
namespace aarch32 {

class ITBlock {
  Condition first_condition_;
  Condition condition_;
  uint16_t it_mask_;

 public:
  ITBlock() : first_condition_(al), condition_(al), it_mask_(0) {}
  void Advance() {
    condition_ = Condition((condition_.GetCondition() & 0xe) | (it_mask_ >> 3));
    it_mask_ = (it_mask_ << 1) & 0xf;
  }
  bool InITBlock() const { return it_mask_ != 0; }
  bool OutsideITBlock() const { return !InITBlock(); }
  bool LastInITBlock() const { return it_mask_ == 0x8; }
  bool OutsideITBlockOrLast() const {
    return OutsideITBlock() || LastInITBlock();
  }
  void Set(Condition first_condition, uint16_t mask) {
    condition_ = first_condition_ = first_condition;
    it_mask_ = mask;
  }
  Condition GetFirstCondition() const { return first_condition_; }
  Condition GetCurrentCondition() const { return condition_; }
};

class Disassembler {
 public:
  enum LocationType {
    kAnyLocation,
    kCodeLocation,
    kDataLocation,
    kCoprocLocation,
    kLoadByteLocation,
    kLoadHalfWordLocation,
    kLoadWordLocation,
    kLoadDoubleWordLocation,
    kLoadSignedByteLocation,
    kLoadSignedHalfWordLocation,
    kLoadSinglePrecisionLocation,
    kLoadDoublePrecisionLocation,
    kStoreByteLocation,
    kStoreHalfWordLocation,
    kStoreWordLocation,
    kStoreDoubleWordLocation,
    kStoreSinglePrecisionLocation,
    kStoreDoublePrecisionLocation,
    kVld1Location,
    kVld2Location,
    kVld3Location,
    kVld4Location,
    kVst1Location,
    kVst2Location,
    kVst3Location,
    kVst4Location
  };

  class ConditionPrinter {
    const ITBlock& it_block_;
    Condition cond_;

   public:
    ConditionPrinter(const ITBlock& it_block, Condition cond)
        : it_block_(it_block), cond_(cond) {}
    const ITBlock& GetITBlock() const { return it_block_; }
    Condition GetCond() const { return cond_; }
    friend std::ostream& operator<<(std::ostream& os, ConditionPrinter cond) {
      if (cond.it_block_.InITBlock() && cond.cond_.Is(al) &&
          !cond.cond_.IsNone()) {
        return os << "al";
      }
      return os << cond.cond_;
    }
  };

  class ImmediatePrinter {
    uint32_t imm_;

   public:
    explicit ImmediatePrinter(uint32_t imm) : imm_(imm) {}
    uint32_t GetImm() const { return imm_; }
    friend std::ostream& operator<<(std::ostream& os, ImmediatePrinter imm) {
      return os << "#" << imm.GetImm();
    }
  };

  class SignedImmediatePrinter {
    int32_t imm_;

   public:
    explicit SignedImmediatePrinter(int32_t imm) : imm_(imm) {}
    int32_t GetImm() const { return imm_; }
    friend std::ostream& operator<<(std::ostream& os,
                                    SignedImmediatePrinter imm) {
      return os << "#" << imm.GetImm();
    }
  };

  class RawImmediatePrinter {
    uint32_t imm_;

   public:
    explicit RawImmediatePrinter(uint32_t imm) : imm_(imm) {}
    uint32_t GetImm() const { return imm_; }
    friend std::ostream& operator<<(std::ostream& os, RawImmediatePrinter imm) {
      return os << imm.GetImm();
    }
  };

  class DtPrinter {
    DataType dt_;
    DataType default_dt_;

   public:
    DtPrinter(DataType dt, DataType default_dt)
        : dt_(dt), default_dt_(default_dt) {}
    DataType GetDt() const { return dt_; }
    DataType GetDefaultDt() const { return default_dt_; }
    friend std::ostream& operator<<(std::ostream& os, DtPrinter dt) {
      if (dt.dt_.Is(dt.default_dt_)) return os;
      return os << dt.dt_;
    }
  };

  class IndexedRegisterPrinter {
    DRegister reg_;
    uint32_t index_;

   public:
    IndexedRegisterPrinter(DRegister reg, uint32_t index)
        : reg_(reg), index_(index) {}
    DRegister GetReg() const { return reg_; }
    uint32_t GetIndex() const { return index_; }
    friend std::ostream& operator<<(std::ostream& os,
                                    IndexedRegisterPrinter reg) {
      return os << reg.GetReg() << "[" << reg.GetIndex() << "]";
    }
  };

  // TODO: Merge this class with PrintLabel below. This Location class
  // represents a PC-relative offset, not an address.
  class Location {
   public:
    typedef int32_t Offset;

    Location(Offset immediate, Offset pc_offset)
        : immediate_(immediate), pc_offset_(pc_offset) {}
    Offset GetImmediate() const { return immediate_; }
    Offset GetPCOffset() const { return pc_offset_; }

   private:
    Offset immediate_;
    Offset pc_offset_;
  };

  class PrintLabel {
    LocationType location_type_;
    Location::Offset immediate_;
    Location::Offset location_;

   public:
    PrintLabel(LocationType location_type,
               Location* offset,
               Location::Offset position)
        : location_type_(location_type),
          immediate_(offset->GetImmediate()),
          location_(static_cast<Location::Offset>(
              static_cast<int64_t>(offset->GetPCOffset()) +
              offset->GetImmediate() + position)) {}

    LocationType GetLocationType() const { return location_type_; }
    Location::Offset GetLocation() const { return location_; }
    Location::Offset GetImmediate() const { return immediate_; }

    friend inline std::ostream& operator<<(std::ostream& os,
                                           const PrintLabel& label) {
      os << "0x" << std::hex << std::setw(8) << std::setfill('0')
         << label.GetLocation() << std::dec;
      return os;
    }
  };


  class PrintMemOperand {
    LocationType location_type_;
    const MemOperand& operand_;

   public:
    PrintMemOperand(LocationType location_type, const MemOperand& operand)
        : location_type_(location_type), operand_(operand) {}
    LocationType GetLocationType() const { return location_type_; }
    const MemOperand& GetOperand() const { return operand_; }
  };

  class PrintAlignedMemOperand {
    LocationType location_type_;
    const AlignedMemOperand& operand_;

   public:
    PrintAlignedMemOperand(LocationType location_type,
                           const AlignedMemOperand& operand)
        : location_type_(location_type), operand_(operand) {}
    LocationType GetLocationType() const { return location_type_; }
    const AlignedMemOperand& GetOperand() const { return operand_; }
  };

  class DisassemblerStream {
    std::ostream& os_;
    InstructionType current_instruction_type_;
    InstructionAttribute current_instruction_attributes_;

   public:
    explicit DisassemblerStream(std::ostream& os)  // NOLINT(runtime/references)
        : os_(os),
          current_instruction_type_(kUndefInstructionType),
          current_instruction_attributes_(kNoAttribute) {}
    virtual ~DisassemblerStream() {}
    std::ostream& os() const { return os_; }
    void SetCurrentInstruction(
        InstructionType current_instruction_type,
        InstructionAttribute current_instruction_attributes) {
      current_instruction_type_ = current_instruction_type;
      current_instruction_attributes_ = current_instruction_attributes;
    }
    InstructionType GetCurrentInstructionType() const {
      return current_instruction_type_;
    }
    InstructionAttribute GetCurrentInstructionAttributes() const {
      return current_instruction_attributes_;
    }
    bool Has(InstructionAttribute attributes) const {
      return (current_instruction_attributes_ & attributes) == attributes;
    }
    template <typename T>
    DisassemblerStream& operator<<(T value) {
      os_ << value;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const char* string) {
      os_ << string;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const ConditionPrinter& cond) {
      os_ << cond;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Condition cond) {
      os_ << cond;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const EncodingSize& size) {
      os_ << size;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const ImmediatePrinter& imm) {
      os_ << imm;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const SignedImmediatePrinter& imm) {
      os_ << imm;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const RawImmediatePrinter& imm) {
      os_ << imm;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const DtPrinter& dt) {
      os_ << dt;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const DataType& type) {
      os_ << type;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Shift shift) {
      os_ << shift;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Sign sign) {
      os_ << sign;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Alignment alignment) {
      os_ << alignment;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const PrintLabel& label) {
      os_ << label;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const WriteBack& write_back) {
      os_ << write_back;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const NeonImmediate& immediate) {
      os_ << immediate;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Register reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(SRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(DRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(QRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const RegisterOrAPSR_nzcv reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(SpecialRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(MaskedSpecialRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(SpecialFPRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(BankedRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const RegisterList& list) {
      os_ << list;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const SRegisterList& list) {
      os_ << list;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const DRegisterList& list) {
      os_ << list;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const NeonRegisterList& list) {
      os_ << list;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const DRegisterLane& reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const IndexedRegisterPrinter& reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Coprocessor coproc) {
      os_ << coproc;
      return *this;
    }
    virtual DisassemblerStream& operator<<(CRegister reg) {
      os_ << reg;
      return *this;
    }
    virtual DisassemblerStream& operator<<(Endianness endian_specifier) {
      os_ << endian_specifier;
      return *this;
    }
    virtual DisassemblerStream& operator<<(MemoryBarrier option) {
      os_ << option;
      return *this;
    }
    virtual DisassemblerStream& operator<<(InterruptFlags iflags) {
      os_ << iflags;
      return *this;
    }
    virtual DisassemblerStream& operator<<(const Operand& operand) {
      if (operand.IsImmediate()) {
        if (Has(kBitwise)) {
          return *this << "#0x" << std::hex << operand.GetImmediate()
                       << std::dec;
        }
        return *this << "#" << operand.GetImmediate();
      }
      if (operand.IsImmediateShiftedRegister()) {
        if ((operand.GetShift().IsLSL() || operand.GetShift().IsROR()) &&
            (operand.GetShiftAmount() == 0)) {
          return *this << operand.GetBaseRegister();
        }
        if (operand.GetShift().IsRRX()) {
          return *this << operand.GetBaseRegister() << ", rrx";
        }
        return *this << operand.GetBaseRegister() << ", " << operand.GetShift()
                     << " #" << operand.GetShiftAmount();
      }
      if (operand.IsRegisterShiftedRegister()) {
        return *this << operand.GetBaseRegister() << ", " << operand.GetShift()
                     << " " << operand.GetShiftRegister();
      }
      VIXL_UNREACHABLE();
      return *this;
    }
    virtual DisassemblerStream& operator<<(const SOperand& operand) {
      if (operand.IsImmediate()) {
        return *this << operand.GetNeonImmediate();
      }
      return *this << operand.GetRegister();
    }
    virtual DisassemblerStream& operator<<(const DOperand& operand) {
      if (operand.IsImmediate()) {
        return *this << operand.GetNeonImmediate();
      }
      return *this << operand.GetRegister();
    }
    virtual DisassemblerStream& operator<<(const QOperand& operand) {
      if (operand.IsImmediate()) {
        return *this << operand.GetNeonImmediate();
      }
      return *this << operand.GetRegister();
    }
    virtual DisassemblerStream& operator<<(const MemOperand& operand) {
      *this << "[" << operand.GetBaseRegister();
      if (operand.GetAddrMode() == PostIndex) {
        *this << "]";
        if (operand.IsRegisterOnly()) return *this << "!";
      }
      if (operand.IsImmediate()) {
        if ((operand.GetOffsetImmediate() != 0) ||
            operand.GetSign().IsMinus() ||
            ((operand.GetAddrMode() != Offset) && !operand.IsRegisterOnly())) {
          if (operand.GetOffsetImmediate() == 0) {
            *this << ", #" << operand.GetSign() << operand.GetOffsetImmediate();
          } else {
            *this << ", #" << operand.GetOffsetImmediate();
          }
        }
      } else if (operand.IsPlainRegister()) {
        *this << ", " << operand.GetSign() << operand.GetOffsetRegister();
      } else if (operand.IsShiftedRegister()) {
        *this << ", " << operand.GetSign() << operand.GetOffsetRegister()
              << ImmediateShiftOperand(operand.GetShift(),
                                       operand.GetShiftAmount());
      } else {
        VIXL_UNREACHABLE();
        return *this;
      }
      if (operand.GetAddrMode() == Offset) {
        *this << "]";
      } else if (operand.GetAddrMode() == PreIndex) {
        *this << "]!";
      }
      return *this;
    }
    virtual DisassemblerStream& operator<<(const PrintMemOperand& operand) {
      return *this << operand.GetOperand();
    }
    virtual DisassemblerStream& operator<<(const AlignedMemOperand& operand) {
      *this << "[" << operand.GetBaseRegister() << operand.GetAlignment()
            << "]";
      if (operand.GetAddrMode() == PostIndex) {
        if (operand.IsPlainRegister()) {
          *this << ", " << operand.GetOffsetRegister();
        } else {
          *this << "!";
        }
      }
      return *this;
    }
    virtual DisassemblerStream& operator<<(
        const PrintAlignedMemOperand& operand) {
      return *this << operand.GetOperand();
    }
  };

 private:
  class ITBlockScope {
    ITBlock* const it_block_;
    bool inside_;

   public:
    explicit ITBlockScope(ITBlock* it_block)
        : it_block_(it_block), inside_(it_block->InITBlock()) {}
    ~ITBlockScope() {
      if (inside_) it_block_->Advance();
    }
  };

  ITBlock it_block_;
  DisassemblerStream* os_;
  bool owns_os_;
  uint32_t code_address_;
  // True if the disassembler always output instructions with all the
  // registers (even if two registers are identical and only one could be
  // output).
  bool use_short_hand_form_;

 public:
  explicit Disassembler(std::ostream& os,  // NOLINT(runtime/references)
                        uint32_t code_address = 0)
      : os_(new DisassemblerStream(os)),
        owns_os_(true),
        code_address_(code_address),
        use_short_hand_form_(true) {}
  explicit Disassembler(DisassemblerStream* os, uint32_t code_address = 0)
      : os_(os),
        owns_os_(false),
        code_address_(code_address),
        use_short_hand_form_(true) {}
  virtual ~Disassembler() {
    if (owns_os_) {
      delete os_;
    }
  }
  DisassemblerStream& os() const { return *os_; }
  void SetIT(Condition first_condition, uint16_t it_mask) {
    it_block_.Set(first_condition, it_mask);
  }
  const ITBlock& GetITBlock() const { return it_block_; }
  bool InITBlock() const { return it_block_.InITBlock(); }
  bool OutsideITBlock() const { return it_block_.OutsideITBlock(); }
  bool OutsideITBlockOrLast() const { return it_block_.OutsideITBlockOrLast(); }
  void CheckNotIT() const { VIXL_ASSERT(it_block_.OutsideITBlock()); }
  // Return the current condition depending on the IT state for T32.
  Condition CurrentCond() const {
    if (it_block_.OutsideITBlock()) return al;
    return it_block_.GetCurrentCondition();
  }
  bool UseShortHandForm() const { return use_short_hand_form_; }
  void SetUseShortHandForm(bool use_short_hand_form) {
    use_short_hand_form_ = use_short_hand_form;
  }

  virtual void UnallocatedT32(uint32_t instruction) {
    if (T32Size(instruction) == 2) {
      os() << "unallocated " << std::hex << std::setw(4) << std::setfill('0')
           << (instruction >> 16) << std::dec;
    } else {
      os() << "unallocated " << std::hex << std::setw(8) << std::setfill('0')
           << instruction << std::dec;
    }
  }
  virtual void UnallocatedA32(uint32_t instruction) {
    os() << "unallocated " << std::hex << std::setw(8) << std::setfill('0')
         << instruction << std::dec;
  }
  virtual void UnimplementedT32_16(const char* name, uint32_t instruction) {
    os() << "unimplemented " << name << " T32:" << std::hex << std::setw(4)
         << std::setfill('0') << (instruction >> 16) << std::dec;
  }
  virtual void UnimplementedT32_32(const char* name, uint32_t instruction) {
    os() << "unimplemented " << name << " T32:" << std::hex << std::setw(8)
         << std::setfill('0') << instruction << std::dec;
  }
  virtual void UnimplementedA32(const char* name, uint32_t instruction) {
    os() << "unimplemented " << name << " ARM:" << std::hex << std::setw(8)
         << std::setfill('0') << instruction << std::dec;
  }
  virtual void Unpredictable() { os() << " ; unpredictable"; }
  virtual void UnpredictableT32(uint32_t /*instr*/) { return Unpredictable(); }
  virtual void UnpredictableA32(uint32_t /*instr*/) { return Unpredictable(); }

  static bool Is16BitEncoding(uint32_t instr) { return instr < 0xe8000000; }
  uint32_t GetCodeAddress() const { return code_address_; }
  void SetCodeAddress(uint32_t code_address) { code_address_ = code_address; }

  // Start of generated code.

  void adc(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void adcs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void add(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void add(Condition cond, Register rd, const Operand& operand);

  void adds(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void adds(Register rd, const Operand& operand);

  void addw(Condition cond, Register rd, Register rn, const Operand& operand);

  void adr(Condition cond, EncodingSize size, Register rd, Location* location);

  void and_(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void ands(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void asr(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);

  void asrs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);

  void b(Condition cond, EncodingSize size, Location* location);

  void bfc(Condition cond, Register rd, uint32_t lsb, uint32_t width);

  void bfi(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);

  void bic(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void bics(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void bkpt(Condition cond, uint32_t imm);

  void bl(Condition cond, Location* location);

  void blx(Condition cond, Location* location);

  void blx(Condition cond, Register rm);

  void bx(Condition cond, Register rm);

  void bxj(Condition cond, Register rm);

  void cbnz(Register rn, Location* location);

  void cbz(Register rn, Location* location);

  void clrex(Condition cond);

  void clz(Condition cond, Register rd, Register rm);

  void cmn(Condition cond,
           EncodingSize size,
           Register rn,
           const Operand& operand);

  void cmp(Condition cond,
           EncodingSize size,
           Register rn,
           const Operand& operand);

  void crc32b(Condition cond, Register rd, Register rn, Register rm);

  void crc32cb(Condition cond, Register rd, Register rn, Register rm);

  void crc32ch(Condition cond, Register rd, Register rn, Register rm);

  void crc32cw(Condition cond, Register rd, Register rn, Register rm);

  void crc32h(Condition cond, Register rd, Register rn, Register rm);

  void crc32w(Condition cond, Register rd, Register rn, Register rm);

  void dmb(Condition cond, MemoryBarrier option);

  void dsb(Condition cond, MemoryBarrier option);

  void eor(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void eors(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void fldmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);

  void fldmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);

  void fstmdbx(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);

  void fstmiax(Condition cond,
               Register rn,
               WriteBack write_back,
               DRegisterList dreglist);

  void hlt(Condition cond, uint32_t imm);

  void hvc(Condition cond, uint32_t imm);

  void isb(Condition cond, MemoryBarrier option);

  void it(Condition cond, uint16_t mask);

  void lda(Condition cond, Register rt, const MemOperand& operand);

  void ldab(Condition cond, Register rt, const MemOperand& operand);

  void ldaex(Condition cond, Register rt, const MemOperand& operand);

  void ldaexb(Condition cond, Register rt, const MemOperand& operand);

  void ldaexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand);

  void ldaexh(Condition cond, Register rt, const MemOperand& operand);

  void ldah(Condition cond, Register rt, const MemOperand& operand);

  void ldm(Condition cond,
           EncodingSize size,
           Register rn,
           WriteBack write_back,
           RegisterList registers);

  void ldmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldmdb(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldmea(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldmfd(Condition cond,
             EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void ldr(Condition cond,
           EncodingSize size,
           Register rt,
           const MemOperand& operand);

  void ldr(Condition cond, EncodingSize size, Register rt, Location* location);

  void ldrb(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);

  void ldrb(Condition cond, Register rt, Location* location);

  void ldrd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand);

  void ldrd(Condition cond, Register rt, Register rt2, Location* location);

  void ldrex(Condition cond, Register rt, const MemOperand& operand);

  void ldrexb(Condition cond, Register rt, const MemOperand& operand);

  void ldrexd(Condition cond,
              Register rt,
              Register rt2,
              const MemOperand& operand);

  void ldrexh(Condition cond, Register rt, const MemOperand& operand);

  void ldrh(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);

  void ldrh(Condition cond, Register rt, Location* location);

  void ldrsb(Condition cond,
             EncodingSize size,
             Register rt,
             const MemOperand& operand);

  void ldrsb(Condition cond, Register rt, Location* location);

  void ldrsh(Condition cond,
             EncodingSize size,
             Register rt,
             const MemOperand& operand);

  void ldrsh(Condition cond, Register rt, Location* location);

  void lsl(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);

  void lsls(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);

  void lsr(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);

  void lsrs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);

  void mla(Condition cond, Register rd, Register rn, Register rm, Register ra);

  void mlas(Condition cond, Register rd, Register rn, Register rm, Register ra);

  void mls(Condition cond, Register rd, Register rn, Register rm, Register ra);

  void mov(Condition cond,
           EncodingSize size,
           Register rd,
           const Operand& operand);

  void movs(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);

  void movt(Condition cond, Register rd, const Operand& operand);

  void movw(Condition cond, Register rd, const Operand& operand);

  void mrs(Condition cond, Register rd, SpecialRegister spec_reg);

  void msr(Condition cond,
           MaskedSpecialRegister spec_reg,
           const Operand& operand);

  void mul(
      Condition cond, EncodingSize size, Register rd, Register rn, Register rm);

  void muls(Condition cond, Register rd, Register rn, Register rm);

  void mvn(Condition cond,
           EncodingSize size,
           Register rd,
           const Operand& operand);

  void mvns(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);

  void nop(Condition cond, EncodingSize size);

  void orn(Condition cond, Register rd, Register rn, const Operand& operand);

  void orns(Condition cond, Register rd, Register rn, const Operand& operand);

  void orr(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void orrs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void pkhbt(Condition cond, Register rd, Register rn, const Operand& operand);

  void pkhtb(Condition cond, Register rd, Register rn, const Operand& operand);

  void pld(Condition cond, Location* location);

  void pld(Condition cond, const MemOperand& operand);

  void pldw(Condition cond, const MemOperand& operand);

  void pli(Condition cond, const MemOperand& operand);

  void pli(Condition cond, Location* location);

  void pop(Condition cond, EncodingSize size, RegisterList registers);

  void pop(Condition cond, EncodingSize size, Register rt);

  void push(Condition cond, EncodingSize size, RegisterList registers);

  void push(Condition cond, EncodingSize size, Register rt);

  void qadd(Condition cond, Register rd, Register rm, Register rn);

  void qadd16(Condition cond, Register rd, Register rn, Register rm);

  void qadd8(Condition cond, Register rd, Register rn, Register rm);

  void qasx(Condition cond, Register rd, Register rn, Register rm);

  void qdadd(Condition cond, Register rd, Register rm, Register rn);

  void qdsub(Condition cond, Register rd, Register rm, Register rn);

  void qsax(Condition cond, Register rd, Register rn, Register rm);

  void qsub(Condition cond, Register rd, Register rm, Register rn);

  void qsub16(Condition cond, Register rd, Register rn, Register rm);

  void qsub8(Condition cond, Register rd, Register rn, Register rm);

  void rbit(Condition cond, Register rd, Register rm);

  void rev(Condition cond, EncodingSize size, Register rd, Register rm);

  void rev16(Condition cond, EncodingSize size, Register rd, Register rm);

  void revsh(Condition cond, EncodingSize size, Register rd, Register rm);

  void ror(Condition cond,
           EncodingSize size,
           Register rd,
           Register rm,
           const Operand& operand);

  void rors(Condition cond,
            EncodingSize size,
            Register rd,
            Register rm,
            const Operand& operand);

  void rrx(Condition cond, Register rd, Register rm);

  void rrxs(Condition cond, Register rd, Register rm);

  void rsb(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void rsbs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void rsc(Condition cond, Register rd, Register rn, const Operand& operand);

  void rscs(Condition cond, Register rd, Register rn, const Operand& operand);

  void sadd16(Condition cond, Register rd, Register rn, Register rm);

  void sadd8(Condition cond, Register rd, Register rn, Register rm);

  void sasx(Condition cond, Register rd, Register rn, Register rm);

  void sbc(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void sbcs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void sbfx(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);

  void sdiv(Condition cond, Register rd, Register rn, Register rm);

  void sel(Condition cond, Register rd, Register rn, Register rm);

  void shadd16(Condition cond, Register rd, Register rn, Register rm);

  void shadd8(Condition cond, Register rd, Register rn, Register rm);

  void shasx(Condition cond, Register rd, Register rn, Register rm);

  void shsax(Condition cond, Register rd, Register rn, Register rm);

  void shsub16(Condition cond, Register rd, Register rn, Register rm);

  void shsub8(Condition cond, Register rd, Register rn, Register rm);

  void smlabb(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlabt(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlad(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smladx(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlalbb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlalbt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlald(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlaldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlaltb(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlaltt(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlatb(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlatt(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlawb(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlawt(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlsd(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlsdx(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smlsld(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smlsldx(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smmla(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smmlar(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smmls(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smmlsr(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void smmul(Condition cond, Register rd, Register rn, Register rm);

  void smmulr(Condition cond, Register rd, Register rn, Register rm);

  void smuad(Condition cond, Register rd, Register rn, Register rm);

  void smuadx(Condition cond, Register rd, Register rn, Register rm);

  void smulbb(Condition cond, Register rd, Register rn, Register rm);

  void smulbt(Condition cond, Register rd, Register rn, Register rm);

  void smull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void smultb(Condition cond, Register rd, Register rn, Register rm);

  void smultt(Condition cond, Register rd, Register rn, Register rm);

  void smulwb(Condition cond, Register rd, Register rn, Register rm);

  void smulwt(Condition cond, Register rd, Register rn, Register rm);

  void smusd(Condition cond, Register rd, Register rn, Register rm);

  void smusdx(Condition cond, Register rd, Register rn, Register rm);

  void ssat(Condition cond, Register rd, uint32_t imm, const Operand& operand);

  void ssat16(Condition cond, Register rd, uint32_t imm, Register rn);

  void ssax(Condition cond, Register rd, Register rn, Register rm);

  void ssub16(Condition cond, Register rd, Register rn, Register rm);

  void ssub8(Condition cond, Register rd, Register rn, Register rm);

  void stl(Condition cond, Register rt, const MemOperand& operand);

  void stlb(Condition cond, Register rt, const MemOperand& operand);

  void stlex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand);

  void stlexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);

  void stlexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand);

  void stlexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);

  void stlh(Condition cond, Register rt, const MemOperand& operand);

  void stm(Condition cond,
           EncodingSize size,
           Register rn,
           WriteBack write_back,
           RegisterList registers);

  void stmda(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void stmdb(Condition cond,
             EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void stmea(Condition cond,
             EncodingSize size,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void stmed(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void stmfa(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void stmfd(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void stmib(Condition cond,
             Register rn,
             WriteBack write_back,
             RegisterList registers);

  void str(Condition cond,
           EncodingSize size,
           Register rt,
           const MemOperand& operand);

  void strb(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);

  void strd(Condition cond,
            Register rt,
            Register rt2,
            const MemOperand& operand);

  void strex(Condition cond,
             Register rd,
             Register rt,
             const MemOperand& operand);

  void strexb(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);

  void strexd(Condition cond,
              Register rd,
              Register rt,
              Register rt2,
              const MemOperand& operand);

  void strexh(Condition cond,
              Register rd,
              Register rt,
              const MemOperand& operand);

  void strh(Condition cond,
            EncodingSize size,
            Register rt,
            const MemOperand& operand);

  void sub(Condition cond,
           EncodingSize size,
           Register rd,
           Register rn,
           const Operand& operand);

  void sub(Condition cond, Register rd, const Operand& operand);

  void subs(Condition cond,
            EncodingSize size,
            Register rd,
            Register rn,
            const Operand& operand);

  void subs(Register rd, const Operand& operand);

  void subw(Condition cond, Register rd, Register rn, const Operand& operand);

  void svc(Condition cond, uint32_t imm);

  void sxtab(Condition cond, Register rd, Register rn, const Operand& operand);

  void sxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand);

  void sxtah(Condition cond, Register rd, Register rn, const Operand& operand);

  void sxtb(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);

  void sxtb16(Condition cond, Register rd, const Operand& operand);

  void sxth(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);

  void tbb(Condition cond, Register rn, Register rm);

  void tbh(Condition cond, Register rn, Register rm);

  void teq(Condition cond, Register rn, const Operand& operand);

  void tst(Condition cond,
           EncodingSize size,
           Register rn,
           const Operand& operand);

  void uadd16(Condition cond, Register rd, Register rn, Register rm);

  void uadd8(Condition cond, Register rd, Register rn, Register rm);

  void uasx(Condition cond, Register rd, Register rn, Register rm);

  void ubfx(
      Condition cond, Register rd, Register rn, uint32_t lsb, uint32_t width);

  void udf(Condition cond, EncodingSize size, uint32_t imm);

  void udiv(Condition cond, Register rd, Register rn, Register rm);

  void uhadd16(Condition cond, Register rd, Register rn, Register rm);

  void uhadd8(Condition cond, Register rd, Register rn, Register rm);

  void uhasx(Condition cond, Register rd, Register rn, Register rm);

  void uhsax(Condition cond, Register rd, Register rn, Register rm);

  void uhsub16(Condition cond, Register rd, Register rn, Register rm);

  void uhsub8(Condition cond, Register rd, Register rn, Register rm);

  void umaal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void umlal(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void umlals(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void umull(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void umulls(
      Condition cond, Register rdlo, Register rdhi, Register rn, Register rm);

  void uqadd16(Condition cond, Register rd, Register rn, Register rm);

  void uqadd8(Condition cond, Register rd, Register rn, Register rm);

  void uqasx(Condition cond, Register rd, Register rn, Register rm);

  void uqsax(Condition cond, Register rd, Register rn, Register rm);

  void uqsub16(Condition cond, Register rd, Register rn, Register rm);

  void uqsub8(Condition cond, Register rd, Register rn, Register rm);

  void usad8(Condition cond, Register rd, Register rn, Register rm);

  void usada8(
      Condition cond, Register rd, Register rn, Register rm, Register ra);

  void usat(Condition cond, Register rd, uint32_t imm, const Operand& operand);

  void usat16(Condition cond, Register rd, uint32_t imm, Register rn);

  void usax(Condition cond, Register rd, Register rn, Register rm);

  void usub16(Condition cond, Register rd, Register rn, Register rm);

  void usub8(Condition cond, Register rd, Register rn, Register rm);

  void uxtab(Condition cond, Register rd, Register rn, const Operand& operand);

  void uxtab16(Condition cond,
               Register rd,
               Register rn,
               const Operand& operand);

  void uxtah(Condition cond, Register rd, Register rn, const Operand& operand);

  void uxtb(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);

  void uxtb16(Condition cond, Register rd, const Operand& operand);

  void uxth(Condition cond,
            EncodingSize size,
            Register rd,
            const Operand& operand);

  void vaba(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vaba(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vabal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vabd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vabd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vabdl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vabs(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vabs(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vabs(Condition cond, DataType dt, SRegister rd, SRegister rm);

  void vacge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vacge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vacgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vacgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vacle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vacle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vaclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vaclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vadd(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vaddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);

  void vaddl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vaddw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm);

  void vand(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);

  void vand(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);

  void vbic(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);

  void vbic(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);

  void vbif(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vbif(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vbit(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vbit(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vbsl(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vbsl(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vceq(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vceq(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vceq(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vceq(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vcge(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vcge(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vcge(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vcge(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vcgt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vcgt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vcgt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vcgt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vcle(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vcle(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vcle(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vcle(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vcls(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vcls(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vclt(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vclt(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vclt(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vclt(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vclz(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vclz(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vcmp(Condition cond, DataType dt, SRegister rd, const SOperand& operand);

  void vcmp(Condition cond, DataType dt, DRegister rd, const DOperand& operand);

  void vcmpe(Condition cond,
             DataType dt,
             SRegister rd,
             const SOperand& operand);

  void vcmpe(Condition cond,
             DataType dt,
             DRegister rd,
             const DOperand& operand);

  void vcnt(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vcnt(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            DRegister rd,
            DRegister rm,
            int32_t fbits);

  void vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            QRegister rd,
            QRegister rm,
            int32_t fbits);

  void vcvt(Condition cond,
            DataType dt1,
            DataType dt2,
            SRegister rd,
            SRegister rm,
            int32_t fbits);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, DRegister rm);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, QRegister rm);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, QRegister rm);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, QRegister rd, DRegister rm);

  void vcvt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvta(DataType dt1, DataType dt2, DRegister rd, DRegister rm);

  void vcvta(DataType dt1, DataType dt2, QRegister rd, QRegister rm);

  void vcvta(DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvta(DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvtb(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);

  void vcvtb(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);

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

  void vcvtr(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, SRegister rm);

  void vcvtt(
      Condition cond, DataType dt1, DataType dt2, DRegister rd, SRegister rm);

  void vcvtt(
      Condition cond, DataType dt1, DataType dt2, SRegister rd, DRegister rm);

  void vdiv(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vdiv(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vdup(Condition cond, DataType dt, QRegister rd, Register rt);

  void vdup(Condition cond, DataType dt, DRegister rd, Register rt);

  void vdup(Condition cond, DataType dt, DRegister rd, DRegisterLane rm);

  void vdup(Condition cond, DataType dt, QRegister rd, DRegisterLane rm);

  void veor(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void veor(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vext(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister rm,
            const DOperand& operand);

  void vext(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            QRegister rm,
            const QOperand& operand);

  void vfma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vfma(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vfma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vfms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vfms(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vfms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vfnma(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vfnma(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vfnms(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vfnms(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vhsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vhsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vld1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vld2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vld3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand);

  void vld4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist);

  void vldm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist);

  void vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);

  void vldmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);

  void vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);

  void vldmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);

  void vldr(Condition cond, DataType dt, DRegister rd, Location* location);

  void vldr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand);

  void vldr(Condition cond, DataType dt, SRegister rd, Location* location);

  void vldr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand);

  void vmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmax(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vmaxnm(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmaxnm(DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vmaxnm(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmin(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vminnm(DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vminnm(DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vminnm(DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmla(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm);

  void vmla(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm);

  void vmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmla(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmlal(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm);

  void vmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vmls(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegisterLane rm);

  void vmls(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegisterLane rm);

  void vmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmls(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmlsl(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegisterLane rm);

  void vmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vmov(Condition cond, Register rt, SRegister rn);

  void vmov(Condition cond, SRegister rn, Register rt);

  void vmov(Condition cond, Register rt, Register rt2, DRegister rm);

  void vmov(Condition cond, DRegister rm, Register rt, Register rt2);

  void vmov(
      Condition cond, Register rt, Register rt2, SRegister rm, SRegister rm1);

  void vmov(
      Condition cond, SRegister rm, SRegister rm1, Register rt, Register rt2);

  void vmov(Condition cond, DataType dt, DRegisterLane rd, Register rt);

  void vmov(Condition cond, DataType dt, DRegister rd, const DOperand& operand);

  void vmov(Condition cond, DataType dt, QRegister rd, const QOperand& operand);

  void vmov(Condition cond, DataType dt, SRegister rd, const SOperand& operand);

  void vmov(Condition cond, DataType dt, Register rt, DRegisterLane rn);

  void vmovl(Condition cond, DataType dt, QRegister rd, DRegister rm);

  void vmovn(Condition cond, DataType dt, DRegister rd, QRegister rm);

  void vmrs(Condition cond, RegisterOrAPSR_nzcv rt, SpecialFPRegister spec_reg);

  void vmsr(Condition cond, SpecialFPRegister spec_reg, Register rt);

  void vmul(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            DRegister dm,
            unsigned index);

  void vmul(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            DRegister dm,
            unsigned index);

  void vmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vmul(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vmull(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rn,
             DRegister dm,
             unsigned index);

  void vmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vmvn(Condition cond, DataType dt, DRegister rd, const DOperand& operand);

  void vmvn(Condition cond, DataType dt, QRegister rd, const QOperand& operand);

  void vneg(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vneg(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vneg(Condition cond, DataType dt, SRegister rd, SRegister rm);

  void vnmla(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vnmla(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vnmls(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vnmls(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vnmul(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vnmul(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vorn(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);

  void vorn(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);

  void vorr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rn,
            const DOperand& operand);

  void vorr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rn,
            const QOperand& operand);

  void vpadal(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vpadal(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vpadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vpaddl(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vpaddl(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vpmax(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vpmin(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vpop(Condition cond, DataType dt, DRegisterList dreglist);

  void vpop(Condition cond, DataType dt, SRegisterList sreglist);

  void vpush(Condition cond, DataType dt, DRegisterList dreglist);

  void vpush(Condition cond, DataType dt, SRegisterList sreglist);

  void vqabs(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vqabs(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vqadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vqadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vqdmlal(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vqdmlal(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index);

  void vqdmlsl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vqdmlsl(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegister dm,
               unsigned index);

  void vqdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vqdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vqdmulh(Condition cond,
               DataType dt,
               DRegister rd,
               DRegister rn,
               DRegisterLane rm);

  void vqdmulh(Condition cond,
               DataType dt,
               QRegister rd,
               QRegister rn,
               DRegisterLane rm);

  void vqdmull(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vqdmull(Condition cond,
               DataType dt,
               QRegister rd,
               DRegister rn,
               DRegisterLane rm);

  void vqmovn(Condition cond, DataType dt, DRegister rd, QRegister rm);

  void vqmovun(Condition cond, DataType dt, DRegister rd, QRegister rm);

  void vqneg(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vqneg(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vqrdmulh(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vqrdmulh(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vqrdmulh(Condition cond,
                DataType dt,
                DRegister rd,
                DRegister rn,
                DRegisterLane rm);

  void vqrdmulh(Condition cond,
                DataType dt,
                QRegister rd,
                QRegister rn,
                DRegisterLane rm);

  void vqrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn);

  void vqrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn);

  void vqrshrn(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand);

  void vqrshrun(Condition cond,
                DataType dt,
                DRegister rd,
                QRegister rm,
                const QOperand& operand);

  void vqshl(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand);

  void vqshl(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand);

  void vqshlu(Condition cond,
              DataType dt,
              DRegister rd,
              DRegister rm,
              const DOperand& operand);

  void vqshlu(Condition cond,
              DataType dt,
              QRegister rd,
              QRegister rm,
              const QOperand& operand);

  void vqshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand);

  void vqshrun(Condition cond,
               DataType dt,
               DRegister rd,
               QRegister rm,
               const QOperand& operand);

  void vqsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vqsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vraddhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);

  void vrecpe(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrecpe(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vrecps(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vrecps(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vrev16(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrev16(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vrev32(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrev32(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vrev64(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrev64(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vrhadd(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vrhadd(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

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

  void vrintr(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrintx(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrintx(DataType dt, QRegister rd, QRegister rm);

  void vrintx(Condition cond, DataType dt, SRegister rd, SRegister rm);

  void vrintz(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrintz(DataType dt, QRegister rd, QRegister rm);

  void vrintz(Condition cond, DataType dt, SRegister rd, SRegister rm);

  void vrshl(
      Condition cond, DataType dt, DRegister rd, DRegister rm, DRegister rn);

  void vrshl(
      Condition cond, DataType dt, QRegister rd, QRegister rm, QRegister rn);

  void vrshr(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand);

  void vrshr(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand);

  void vrshrn(Condition cond,
              DataType dt,
              DRegister rd,
              QRegister rm,
              const QOperand& operand);

  void vrsqrte(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vrsqrte(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vrsqrts(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vrsqrts(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vrsra(Condition cond,
             DataType dt,
             DRegister rd,
             DRegister rm,
             const DOperand& operand);

  void vrsra(Condition cond,
             DataType dt,
             QRegister rd,
             QRegister rm,
             const QOperand& operand);

  void vrsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);

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

  void vshl(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vshll(Condition cond,
             DataType dt,
             QRegister rd,
             DRegister rm,
             const DOperand& operand);

  void vshr(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vshr(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vshrn(Condition cond,
             DataType dt,
             DRegister rd,
             QRegister rm,
             const QOperand& operand);

  void vsli(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vsli(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vsqrt(Condition cond, DataType dt, SRegister rd, SRegister rm);

  void vsqrt(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vsra(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vsra(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vsri(Condition cond,
            DataType dt,
            DRegister rd,
            DRegister rm,
            const DOperand& operand);

  void vsri(Condition cond,
            DataType dt,
            QRegister rd,
            QRegister rm,
            const QOperand& operand);

  void vst1(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vst2(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vst3(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const MemOperand& operand);

  void vst4(Condition cond,
            DataType dt,
            const NeonRegisterList& nreglist,
            const AlignedMemOperand& operand);

  void vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            DRegisterList dreglist);

  void vstm(Condition cond,
            DataType dt,
            Register rn,
            WriteBack write_back,
            SRegisterList sreglist);

  void vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);

  void vstmdb(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);

  void vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              DRegisterList dreglist);

  void vstmia(Condition cond,
              DataType dt,
              Register rn,
              WriteBack write_back,
              SRegisterList sreglist);

  void vstr(Condition cond,
            DataType dt,
            DRegister rd,
            const MemOperand& operand);

  void vstr(Condition cond,
            DataType dt,
            SRegister rd,
            const MemOperand& operand);

  void vsub(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vsub(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vsub(
      Condition cond, DataType dt, SRegister rd, SRegister rn, SRegister rm);

  void vsubhn(
      Condition cond, DataType dt, DRegister rd, QRegister rn, QRegister rm);

  void vsubl(
      Condition cond, DataType dt, QRegister rd, DRegister rn, DRegister rm);

  void vsubw(
      Condition cond, DataType dt, QRegister rd, QRegister rn, DRegister rm);

  void vswp(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vswp(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vtbl(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm);

  void vtbx(Condition cond,
            DataType dt,
            DRegister rd,
            const NeonRegisterList& nreglist,
            DRegister rm);

  void vtrn(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vtrn(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vtst(
      Condition cond, DataType dt, DRegister rd, DRegister rn, DRegister rm);

  void vtst(
      Condition cond, DataType dt, QRegister rd, QRegister rn, QRegister rm);

  void vuzp(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vuzp(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void vzip(Condition cond, DataType dt, DRegister rd, DRegister rm);

  void vzip(Condition cond, DataType dt, QRegister rd, QRegister rm);

  void yield(Condition cond, EncodingSize size);

  int T32Size(uint32_t instr);
  void DecodeT32(uint32_t instr);
  void DecodeA32(uint32_t instr);
};

DataTypeValue Dt_L_imm6_1_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_L_imm6_2_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_L_imm6_3_Decode(uint32_t value);
DataTypeValue Dt_L_imm6_4_Decode(uint32_t value);
DataTypeValue Dt_imm6_1_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_imm6_2_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_imm6_3_Decode(uint32_t value);
DataTypeValue Dt_imm6_4_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_op_U_size_1_Decode(uint32_t value);
DataTypeValue Dt_op_size_1_Decode(uint32_t value);
DataTypeValue Dt_op_size_2_Decode(uint32_t value);
DataTypeValue Dt_op_size_3_Decode(uint32_t value);
DataTypeValue Dt_U_imm3H_1_Decode(uint32_t value);
DataTypeValue Dt_U_opc1_opc2_1_Decode(uint32_t value, unsigned* lane);
DataTypeValue Dt_opc1_opc2_1_Decode(uint32_t value, unsigned* lane);
DataTypeValue Dt_imm4_1_Decode(uint32_t value, unsigned* lane);
DataTypeValue Dt_B_E_1_Decode(uint32_t value);
DataTypeValue Dt_op_1_Decode1(uint32_t value);
DataTypeValue Dt_op_1_Decode2(uint32_t value);
DataTypeValue Dt_op_2_Decode(uint32_t value);
DataTypeValue Dt_op_3_Decode(uint32_t value);
DataTypeValue Dt_U_sx_1_Decode(uint32_t value);
DataTypeValue Dt_op_U_1_Decode1(uint32_t value);
DataTypeValue Dt_op_U_1_Decode2(uint32_t value);
DataTypeValue Dt_sz_1_Decode(uint32_t value);
DataTypeValue Dt_F_size_1_Decode(uint32_t value);
DataTypeValue Dt_F_size_2_Decode(uint32_t value);
DataTypeValue Dt_F_size_3_Decode(uint32_t value);
DataTypeValue Dt_F_size_4_Decode(uint32_t value);
DataTypeValue Dt_U_size_1_Decode(uint32_t value);
DataTypeValue Dt_U_size_2_Decode(uint32_t value);
DataTypeValue Dt_U_size_3_Decode(uint32_t value);
DataTypeValue Dt_size_1_Decode(uint32_t value);
DataTypeValue Dt_size_2_Decode(uint32_t value);
DataTypeValue Dt_size_3_Decode(uint32_t value);
DataTypeValue Dt_size_4_Decode(uint32_t value);
DataTypeValue Dt_size_5_Decode(uint32_t value);
DataTypeValue Dt_size_6_Decode(uint32_t value);
DataTypeValue Dt_size_7_Decode(uint32_t value);
DataTypeValue Dt_size_8_Decode(uint32_t value);
DataTypeValue Dt_size_9_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_size_10_Decode(uint32_t value);
DataTypeValue Dt_size_11_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_size_12_Decode(uint32_t value, uint32_t type_value);
DataTypeValue Dt_size_13_Decode(uint32_t value);
DataTypeValue Dt_size_14_Decode(uint32_t value);
DataTypeValue Dt_size_15_Decode(uint32_t value);
DataTypeValue Dt_size_16_Decode(uint32_t value);
DataTypeValue Dt_size_17_Decode(uint32_t value);
// End of generated code.

class PrintDisassembler : public Disassembler {
 public:
  explicit PrintDisassembler(std::ostream& os,  // NOLINT(runtime/references)
                             uint32_t code_address = 0)
      : Disassembler(os, code_address) {}
  explicit PrintDisassembler(DisassemblerStream* os, uint32_t code_address = 0)
      : Disassembler(os, code_address) {}

  virtual void PrintCodeAddress(uint32_t code_address) {
    os() << "0x" << std::hex << std::setw(8) << std::setfill('0')
         << code_address << "\t";
  }

  virtual void PrintOpcode16(uint32_t opcode) {
    os() << std::hex << std::setw(4) << std::setfill('0') << opcode << "    "
         << std::dec << "\t";
  }

  virtual void PrintOpcode32(uint32_t opcode) {
    os() << std::hex << std::setw(8) << std::setfill('0') << opcode << std::dec
         << "\t";
  }

  const uint32_t* DecodeA32At(const uint32_t* instruction_address) {
    DecodeA32(*instruction_address);
    return instruction_address + 1;
  }

  // Returns the address of the next instruction.
  const uint16_t* DecodeT32At(const uint16_t* instruction_address,
                              const uint16_t* buffer_end);
  void DecodeT32(uint32_t instruction);
  void DecodeA32(uint32_t instruction);
  void DisassembleA32Buffer(const uint32_t* buffer, size_t size_in_bytes);
  void DisassembleT32Buffer(const uint16_t* buffer, size_t size_in_bytes);
};

}  // namespace aarch32
}  // namespace vixl

#endif  // VIXL_DISASM_AARCH32_H_
