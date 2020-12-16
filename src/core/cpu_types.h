#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <optional>

namespace CPU {

// Memory address mask used for fetching as well as loadstores (removes cached/uncached/user/kernel bits).
enum : PhysicalMemoryAddress
{
  PHYSICAL_MEMORY_ADDRESS_MASK = 0x1FFFFFFF
};
enum : u32
{
  INSTRUCTION_SIZE = sizeof(u32)
};

enum class Segment
{
  KUSEG, // virtual memory
  KSEG0, // physical memory cached
  KSEG1, // physical memory uncached
  KSEG2
};

enum class Reg : u8
{
  zero,
  at,
  v0,
  v1,
  a0,
  a1,
  a2,
  a3,
  t0,
  t1,
  t2,
  t3,
  t4,
  t5,
  t6,
  t7,
  s0,
  s1,
  s2,
  s3,
  s4,
  s5,
  s6,
  s7,
  t8,
  t9,
  k0,
  k1,
  gp,
  sp,
  fp,
  ra,

  // not accessible to instructions
  hi,
  lo,
  pc,
  npc,

  count
};

const char* GetRegName(Reg reg);

enum class InstructionOp : u8
{
  funct = 0,
  b = 1, // i.rt 0 - bltz, 1 - bgez, 16 - bltzal, 17 - bgezal
  j = 2,
  jal = 3,
  beq = 4,
  bne = 5,
  blez = 6,
  bgtz = 7,
  addi = 8,
  addiu = 9,
  slti = 10,
  sltiu = 11,
  andi = 12,
  ori = 13,
  xori = 14,
  lui = 15,
  cop0 = 16,
  cop1 = 17,
  cop2 = 18,
  cop3 = 19,
  lb = 32,
  lh = 33,
  lwl = 34,
  lw = 35,
  lbu = 36,
  lhu = 37,
  lwr = 38,
  sb = 40,
  sh = 41,
  swl = 42,
  sw = 43,
  swr = 46,
  lwc0 = 48,
  lwc1 = 49,
  lwc2 = 50,
  lwc3 = 51,
  swc0 = 56,
  swc1 = 57,
  swc2 = 58,
  swc3 = 59,
};
constexpr u8 INSTRUCTION_COP_BITS = 0x10;
constexpr u8 INSTRUCTION_COP_MASK = 0x3C;
constexpr u8 INSTRUCTION_COP_N_MASK = 0x03;

enum class InstructionFunct : u8
{
  sll = 0,
  srl = 2,
  sra = 3,
  sllv = 4,
  srlv = 6,
  srav = 7,
  jr = 8,
  jalr = 9,
  syscall = 12,
  break_ = 13,
  mfhi = 16,
  mthi = 17,
  mflo = 18,
  mtlo = 19,
  mult = 24,
  multu = 25,
  div = 26,
  divu = 27,
  add = 32,
  addu = 33,
  sub = 34,
  subu = 35,
  and_ = 36,
  or_ = 37,
  xor_ = 38,
  nor = 39,
  slt = 42,
  sltu = 43
};

enum class CopCommonInstruction : u32
{
  mfcn = 0b0000,
  cfcn = 0b0010,
  mtcn = 0b0100,
  ctcn = 0b0110,
  bcnc = 0b1000,
};

enum class Cop0Instruction : u32
{
  tlbr = 0x01,
  tlbwi = 0x02,
  tlbwr = 0x04,
  tlbp = 0x08,
  rfe = 0x10,
};

union Instruction
{
  u32 bits;

  BitField<u32, InstructionOp, 26, 6> op; // function/instruction

  union
  {
    BitField<u32, Reg, 21, 5> rs;
    BitField<u32, Reg, 16, 5> rt;
    BitField<u32, u16, 0, 16> imm;

    ALWAYS_INLINE u32 imm_sext32() const { return SignExtend32(imm.GetValue()); }
    ALWAYS_INLINE u32 imm_zext32() const { return ZeroExtend32(imm.GetValue()); }
  } i;

  union
  {
    BitField<u32, u32, 0, 26> target;
  } j;

  union
  {
    BitField<u32, Reg, 21, 5> rs;
    BitField<u32, Reg, 16, 5> rt;
    BitField<u32, Reg, 11, 5> rd;
    BitField<u32, u8, 6, 5> shamt;
    BitField<u32, InstructionFunct, 0, 6> funct;
  } r;

  union
  {
    u32 bits;
    BitField<u32, u8, 26, 2> cop_n;
    BitField<u32, u16, 0, 16> imm16;
    BitField<u32, u32, 0, 25> imm25;

    ALWAYS_INLINE bool IsCommonInstruction() const { return (bits & (UINT32_C(1) << 25)) == 0; }

    ALWAYS_INLINE CopCommonInstruction CommonOp() const
    {
      return static_cast<CopCommonInstruction>((bits >> 21) & UINT32_C(0b1111));
    }

    ALWAYS_INLINE Cop0Instruction Cop0Op() const { return static_cast<Cop0Instruction>(bits & UINT32_C(0x3F)); }
  } cop;

  bool IsCop2Instruction() const
  {
    return (op == InstructionOp::cop2 || op == InstructionOp::lwc2 || op == InstructionOp::swc2);
  }
};

// Instruction helpers.
bool IsBranchInstruction(const Instruction& instruction);
bool IsUnconditionalBranchInstruction(const Instruction& instruction);
bool IsDirectBranchInstruction(const Instruction& instruction);
u32 GetBranchInstructionTarget(const Instruction& instruction, u32 instruction_pc);
bool IsCallInstruction(const Instruction& instruction);
bool IsReturnInstruction(const Instruction& instruction);
bool IsMemoryLoadInstruction(const Instruction& instruction);
bool IsMemoryStoreInstruction(const Instruction& instruction);
bool InstructionHasLoadDelay(const Instruction& instruction);
bool IsExitBlockInstruction(const Instruction& instruction);
bool CanInstructionTrap(const Instruction& instruction, bool in_user_mode);
bool IsInvalidInstruction(const Instruction& instruction);

struct Registers
{
  union
  {
    u32 r[static_cast<u8>(Reg::count)];

    struct
    {
      u32 zero; // r0
      u32 at;   // r1
      u32 v0;   // r2
      u32 v1;   // r3
      u32 a0;   // r4
      u32 a1;   // r5
      u32 a2;   // r6
      u32 a3;   // r7
      u32 t0;   // r8
      u32 t1;   // r9
      u32 t2;   // r10
      u32 t3;   // r11
      u32 t4;   // r12
      u32 t5;   // r13
      u32 t6;   // r14
      u32 t7;   // r15
      u32 s0;   // r16
      u32 s1;   // r17
      u32 s2;   // r18
      u32 s3;   // r19
      u32 s4;   // r20
      u32 s5;   // r21
      u32 s6;   // r22
      u32 s7;   // r23
      u32 t8;   // r24
      u32 t9;   // r25
      u32 k0;   // r26
      u32 k1;   // r27
      u32 gp;   // r28
      u32 sp;   // r29
      u32 fp;   // r30
      u32 ra;   // r31

      // not accessible to instructions
      u32 hi;
      u32 lo;
      u32 pc;  // at execution time: the address of the next instruction to execute (already fetched)
      u32 npc; // at execution time: the address of the next instruction to fetch
    };
  };
};

std::optional<VirtualMemoryAddress> GetLoadStoreEffectiveAddress(const Instruction& instruction,
                                                                         const Registers* regs);

enum class Cop0Reg : u8
{
  BPC = 3,
  BDA = 5,
  JUMPDEST = 6,
  DCIC = 7,
  BadVaddr = 8,
  BDAM = 9,
  BPCM = 11,
  SR = 12,
  CAUSE = 13,
  EPC = 14,
  PRID = 15
};

enum class Exception : u8
{
  INT = 0x00,     // interrupt
  MOD = 0x01,     // tlb modification
  TLBL = 0x02,    // tlb load
  TLBS = 0x03,    // tlb store
  AdEL = 0x04,    // address error, data load/instruction fetch
  AdES = 0x05,    // address error, data store
  IBE = 0x06,     // bus error on instruction fetch
  DBE = 0x07,     // bus error on data load/store
  Syscall = 0x08, // system call instruction
  BP = 0x09,      // break instruction
  RI = 0x0A,      // reserved instruction
  CpU = 0x0B,     // coprocessor unusable
  Ov = 0x0C,      // arithmetic overflow
};

struct Cop0Registers
{
  u32 BPC;      // breakpoint on execute
  u32 BDA;      // breakpoint on data access
  u32 TAR;      // randomly memorized jump address
  u32 BadVaddr; // bad virtual address value
  u32 BDAM;     // data breakpoint mask
  u32 BPCM;     // execute breakpoint mask
  u32 EPC;      // return address from trap
  u32 PRID;     // processor ID

  union SR
  {
    u32 bits;
    BitField<u32, bool, 0, 1> IEc;  // current interrupt enable
    BitField<u32, bool, 1, 1> KUc;  // current kernel/user mode, user = 1
    BitField<u32, bool, 2, 1> IEp;  // previous interrupt enable
    BitField<u32, bool, 3, 1> KUp;  // previous kernel/user mode, user = 1
    BitField<u32, bool, 4, 1> IEo;  // old interrupt enable
    BitField<u32, bool, 5, 1> KUo;  // old kernel/user mode, user = 1
    BitField<u32, u8, 8, 8> Im;     // interrupt mask, set to 1 = allowed to trigger
    BitField<u32, bool, 16, 1> Isc; // isolate cache, no writes to memory occur
    BitField<u32, bool, 17, 1> Swc; // swap data and instruction caches
    BitField<u32, bool, 18, 1> PZ;  // zero cache parity bits
    BitField<u32, bool, 19, 1> CM;  // last isolated load contains data from memory (tag matches?)
    BitField<u32, bool, 20, 1> PE;  // cache parity error
    BitField<u32, bool, 21, 1> TS;  // tlb shutdown - matched two entries
    BitField<u32, bool, 22, 1> BEV; // boot exception vectors, 0 = KSEG0, 1 = KSEG1
    BitField<u32, bool, 25, 1> RE;  // reverse endianness in user mode
    BitField<u32, bool, 28, 1> CU0; // coprocessor 0 enable in user mode
    BitField<u32, bool, 29, 1> CE1; // coprocessor 1 enable
    BitField<u32, bool, 30, 1> CE2; // coprocessor 2 enable
    BitField<u32, bool, 31, 1> CE3; // coprocessor 3 enable

    BitField<u32, u8, 0, 6> mode_bits;
    BitField<u32, u8, 28, 2> coprocessor_enable_mask;

    static constexpr u32 WRITE_MASK = 0b1111'0010'0111'1111'1111'1111'0011'1111;
  } sr;

  union CAUSE
  {
    u32 bits;
    BitField<u32, Exception, 2, 5> Excode; // which exception occurred
    BitField<u32, u8, 8, 8> Ip;            // interrupt pending
    BitField<u32, u8, 28, 2> CE;           // coprocessor number if caused by a coprocessor
    BitField<u32, bool, 30, 1> BT;         // exception occurred in branch delay slot, and the branch was taken
    BitField<u32, bool, 31, 1> BD;         // exception occurred in branch delay slot, but pushed IP is for branch

    static constexpr u32 WRITE_MASK = 0b0000'0000'0000'0000'0000'0011'0000'0000;
    static constexpr u32 EXCEPTION_WRITE_MASK = 0b1111'0000'0000'0000'0000'0000'0111'1100;

    static u32 MakeValueForException(Exception excode, bool BD, bool BT, u8 CE)
    {
      CAUSE c = {};
      c.Excode = excode;
      c.BD = BD;
      c.BT = BT;
      c.CE = CE;
      return c.bits;
    }
  } cause;

  union DCIC
  {
    u32 bits;
    BitField<u32, bool, 0, 1> status_any_break;
    BitField<u32, bool, 1, 1> status_bpc_code_break;
    BitField<u32, bool, 2, 1> status_bda_data_break;
    BitField<u32, bool, 3, 1> status_bda_data_read_break;
    BitField<u32, bool, 4, 1> status_bda_data_write_break;
    BitField<u32, bool, 5, 1> status_any_jump_break;
    BitField<u32, u8, 12, 2> jump_redirection;
    BitField<u32, bool, 23, 1> super_master_enable_1;
    BitField<u32, bool, 24, 1> execution_breakpoint_enable;
    BitField<u32, bool, 25, 1> data_access_breakpoint;
    BitField<u32, bool, 26, 1> break_on_data_read;
    BitField<u32, bool, 27, 1> break_on_data_write;
    BitField<u32, bool, 28, 1> break_on_any_jump;
    BitField<u32, bool, 29, 1> master_enable_any_jump;
    BitField<u32, bool, 30, 1> master_enable_break;
    BitField<u32, bool, 31, 1> super_master_enable_2;

    static constexpr u32 WRITE_MASK = 0b1111'1111'1000'0000'1111'0000'0011'1111;
  } dcic;
};

} // namespace CPU
