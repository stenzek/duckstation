// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "cpu_code_cache_private.h"
#include "cpu_recompiler_types.h"
#include "cpu_types.h"
#include <array>
#include <bitset>
#include <optional>
#include <utility>
#include <vector>

namespace CPU::NewRec {

// Global options
static constexpr bool EMULATE_LOAD_DELAYS = true;
static constexpr bool SWAP_BRANCH_DELAY_SLOTS = true;

// Arch-specific options
#if defined(CPU_ARCH_X64)
static constexpr u32 NUM_HOST_REGS = 16;
static constexpr bool HAS_MEMORY_OPERANDS = true;
#elif defined(CPU_ARCH_ARM32)
static constexpr u32 NUM_HOST_REGS = 16;
static constexpr bool HAS_MEMORY_OPERANDS = false;
#elif defined(CPU_ARCH_ARM64)
static constexpr u32 NUM_HOST_REGS = 32;
static constexpr bool HAS_MEMORY_OPERANDS = false;
#elif defined(CPU_ARCH_RISCV64)
static constexpr u32 NUM_HOST_REGS = 32;
static constexpr bool HAS_MEMORY_OPERANDS = false;
#endif

// TODO: Get rid of the virtuals... somehow.
class Compiler
{
public:
  Compiler();
  virtual ~Compiler();

  const void* CompileBlock(CodeCache::Block* block, u32* host_code_size, u32* host_far_code_size);

protected:
  enum FlushFlags : u32
  {
    FLUSH_FLUSH_MIPS_REGISTERS = (1 << 0),
    FLUSH_INVALIDATE_MIPS_REGISTERS = (1 << 1),
    FLUSH_FREE_CALLER_SAVED_REGISTERS = (1 << 2),
    FLUSH_FREE_UNNEEDED_CALLER_SAVED_REGISTERS = (1 << 3),
    FLUSH_FREE_ALL_REGISTERS = (1 << 4),
    FLUSH_PC = (1 << 5),
    FLUSH_INSTRUCTION_BITS = (1 << 6),
    FLUSH_CYCLES = (1 << 7),
    FLUSH_LOAD_DELAY = (1 << 8),
    FLUSH_LOAD_DELAY_FROM_STATE = (1 << 9),
    FLUSH_GTE_DONE_CYCLE = (1 << 10),
    FLUSH_GTE_STALL_FROM_STATE = (1 << 11),
    FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS = (1 << 12),

    FLUSH_FOR_C_CALL = (FLUSH_FREE_CALLER_SAVED_REGISTERS),
    FLUSH_FOR_LOADSTORE = (FLUSH_FREE_CALLER_SAVED_REGISTERS | FLUSH_CYCLES),
    FLUSH_FOR_BRANCH = (FLUSH_FLUSH_MIPS_REGISTERS),
    FLUSH_FOR_EXCEPTION =
      (FLUSH_CYCLES | FLUSH_GTE_DONE_CYCLE), // GTE cycles needed because it stalls when a GTE instruction is next.
    FLUSH_FOR_INTERPRETER = (FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_INVALIDATE_MIPS_REGISTERS |
                             FLUSH_FREE_CALLER_SAVED_REGISTERS | FLUSH_PC | FLUSH_CYCLES | FLUSH_INSTRUCTION_BITS |
                             FLUSH_LOAD_DELAY | FLUSH_GTE_DONE_CYCLE | FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS),
    FLUSH_END_BLOCK = 0xFFFFFFFFu & ~(FLUSH_PC | FLUSH_CYCLES | FLUSH_GTE_DONE_CYCLE | FLUSH_INSTRUCTION_BITS |
                                      FLUSH_GTE_STALL_FROM_STATE | FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS),
  };

  union CompileFlags
  {
    struct
    {
      u32 const_s : 1;  // S is constant
      u32 const_t : 1;  // T is constant
      u32 const_lo : 1; // LO is constant
      u32 const_hi : 1; // HI is constant

      u32 valid_host_d : 1;  // D is valid in host register
      u32 valid_host_s : 1;  // S is valid in host register
      u32 valid_host_t : 1;  // T is valid in host register
      u32 valid_host_lo : 1; // LO is valid in host register
      u32 valid_host_hi : 1; // HI is valid in host register

      u32 host_d : 5;  // D host register
      u32 host_s : 5;  // S host register
      u32 host_t : 5;  // T host register
      u32 host_lo : 5; // LO host register

      u32 delay_slot_swapped : 1;
      u32 pad1 : 2; // 28..31

      u32 host_hi : 5; // HI host register

      u32 mips_s : 5; // S guest register
      u32 mips_t : 5; // T guest register

      u32 pad2 : 15; // 32 bits
    };

    u64 bits;

    ALWAYS_INLINE Reg MipsS() const { return static_cast<Reg>(mips_s); }
    ALWAYS_INLINE Reg MipsT() const { return static_cast<Reg>(mips_t); }
  };
  static_assert(sizeof(CompileFlags) == sizeof(u64));

  enum TemplateFlag : u32
  {
    TF_READS_S = (1 << 0),
    TF_READS_T = (1 << 1),
    TF_READS_LO = (1 << 2),
    TF_READS_HI = (1 << 3),
    TF_WRITES_D = (1 << 4),
    TF_WRITES_T = (1 << 5),
    TF_WRITES_LO = (1 << 6),
    TF_WRITES_HI = (1 << 7),
    TF_COMMUTATIVE = (1 << 8), // S op T == T op S
    TF_CAN_OVERFLOW = (1 << 9),

    // TF_NORENAME = // TODO
    TF_LOAD_DELAY = (1 << 10),
    TF_GTE_STALL = (1 << 11),

    TF_NO_NOP = (1 << 12),
    TF_NEEDS_REG_S = (1 << 13),
    TF_NEEDS_REG_T = (1 << 14),
    TF_CAN_SWAP_DELAY_SLOT = (1 << 15),

    TF_RENAME_WITH_ZERO_T = (1 << 16), // add commutative for S as well
    TF_RENAME_WITH_ZERO_IMM = (1 << 17),

    TF_PGXP_WITHOUT_CPU = (1 << 18),
  };

  enum HostRegFlags : u8
  {
    HR_ALLOCATED = (1 << 0),
    HR_NEEDED = (1 << 1),
    HR_MODE_READ = (1 << 2),  // valid
    HR_MODE_WRITE = (1 << 3), // dirty

    HR_USABLE = (1 << 7),
    HR_CALLEE_SAVED = (1 << 6),

    ALLOWED_HR_FLAGS = HR_MODE_READ | HR_MODE_WRITE,
    IMMUTABLE_HR_FLAGS = HR_USABLE | HR_CALLEE_SAVED,
  };

  enum HostRegAllocType : u8
  {
    HR_TYPE_TEMP,
    HR_TYPE_CPU_REG,
    HR_TYPE_PC_WRITEBACK,
    HR_TYPE_LOAD_DELAY_VALUE,
    HR_TYPE_NEXT_LOAD_DELAY_VALUE,
    HR_TYPE_MEMBASE,
  };

  struct HostRegAlloc
  {
    u8 flags;
    HostRegAllocType type;
    Reg reg;
    u16 counter;
  };

  enum class BranchCondition : u8
  {
    Equal,
    NotEqual,
    GreaterThanZero,
    GreaterEqualZero,
    LessThanZero,
    LessEqualZero,
  };

  ALWAYS_INLINE bool HasConstantReg(Reg r) const { return m_constant_regs_valid.test(static_cast<u32>(r)); }
  ALWAYS_INLINE bool HasDirtyConstantReg(Reg r) const { return m_constant_regs_dirty.test(static_cast<u32>(r)); }
  ALWAYS_INLINE bool HasConstantRegValue(Reg r, u32 val) const
  {
    return m_constant_regs_valid.test(static_cast<u32>(r)) && m_constant_reg_values[static_cast<u32>(r)] == val;
  }
  ALWAYS_INLINE u32 GetConstantRegU32(Reg r) const { return m_constant_reg_values[static_cast<u32>(r)]; }
  ALWAYS_INLINE s32 GetConstantRegS32(Reg r) const
  {
    return static_cast<s32>(m_constant_reg_values[static_cast<u32>(r)]);
  }
  void SetConstantReg(Reg r, u32 v);
  void ClearConstantReg(Reg r);
  void FlushConstantReg(Reg r);
  void FlushConstantRegs(bool invalidate);

  Reg MipsD() const;
  u32 GetConditionalBranchTarget(CompileFlags cf) const;
  u32 GetBranchReturnAddress(CompileFlags cf) const;
  bool TrySwapDelaySlot(Reg rs = Reg::zero, Reg rt = Reg::zero, Reg rd = Reg::zero);
  void SetCompilerPC(u32 newpc);
  void TruncateBlock();

  virtual const void* GetCurrentCodePointer() = 0;

  virtual void Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space, u8* far_code_buffer,
                     u32 far_code_space);
  virtual void BeginBlock();
  virtual void GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size) = 0;
  virtual void GenerateICacheCheckAndUpdate() = 0;
  virtual void GenerateCall(const void* func, s32 arg1reg = -1, s32 arg2reg = -1, s32 arg3reg = -1) = 0;
  virtual void EndBlock(const std::optional<u32>& newpc, bool do_event_test) = 0;
  virtual void EndBlockWithException(Exception excode) = 0;
  virtual const void* EndCompile(u32* code_size, u32* far_code_size) = 0;

  ALWAYS_INLINE bool IsHostRegAllocated(u32 r) const { return (m_host_regs[r].flags & HR_ALLOCATED) != 0; }
  static const char* GetReadWriteModeString(u32 flags);
  virtual const char* GetHostRegName(u32 reg) const = 0;
  u32 GetFreeHostReg(u32 flags);
  u32 AllocateHostReg(u32 flags, HostRegAllocType type = HR_TYPE_TEMP, Reg reg = Reg::count);
  std::optional<u32> CheckHostReg(u32 flags, HostRegAllocType type = HR_TYPE_TEMP, Reg reg = Reg::count);
  u32 AllocateTempHostReg(u32 flags = 0);
  void SwapHostRegAlloc(u32 lhs, u32 rhs);
  void FlushHostReg(u32 reg);
  void FreeHostReg(u32 reg);
  void ClearHostReg(u32 reg);
  void MarkRegsNeeded(HostRegAllocType type, Reg reg);
  void RenameHostReg(u32 reg, u32 new_flags, HostRegAllocType new_type, Reg new_reg);
  void ClearHostRegNeeded(u32 reg);
  void ClearHostRegsNeeded();
  void DeleteMIPSReg(Reg reg, bool flush);
  bool TryRenameMIPSReg(Reg to, Reg from, u32 fromhost, Reg other);
  void UpdateHostRegCounters();

  virtual void LoadHostRegWithConstant(u32 reg, u32 val) = 0;
  virtual void LoadHostRegFromCPUPointer(u32 reg, const void* ptr) = 0;
  virtual void StoreConstantToCPUPointer(u32 val, const void* ptr) = 0;
  virtual void StoreHostRegToCPUPointer(u32 reg, const void* ptr) = 0;
  virtual void CopyHostReg(u32 dst, u32 src) = 0;
  virtual void Flush(u32 flags);

  /// Returns true if there is a load delay which will be stored at the end of the instruction.
  bool HasLoadDelay() const { return m_load_delay_register != Reg::count; }

  /// Cancels any pending load delay to the specified register.
  void CancelLoadDelaysToReg(Reg reg);

  /// Moves load delay to the next load delay, and writes any previous load delay to the destination register.
  void UpdateLoadDelay();

  /// Flushes the load delay, i.e. writes it to the destination register.
  void FinishLoadDelay();

  /// Flushes the load delay, but only if it matches the specified register.
  void FinishLoadDelayToReg(Reg reg);

  /// Uses a caller-saved register for load delays when PGXP is enabled.
  u32 GetFlagsForNewLoadDelayedReg() const;

  void BackupHostState();
  void RestoreHostState();

  /// Registers loadstore for possible backpatching.
  void AddLoadStoreInfo(void* code_address, u32 code_size, u32 address_register, u32 data_register,
                        MemoryAccessSize size, bool is_signed, bool is_load);

  void CompileInstruction();
  void CompileBranchDelaySlot(bool dirty_pc = true);

  void CompileTemplate(void (Compiler::*const_func)(CompileFlags), void (Compiler::*func)(CompileFlags),
                       const void* pgxp_cpu_func, u32 tflags);
  void CompileLoadStoreTemplate(void (Compiler::*func)(CompileFlags, MemoryAccessSize, bool, bool,
                                                       const std::optional<VirtualMemoryAddress>&),
                                MemoryAccessSize size, bool store, bool sign, u32 tflags);
  void FlushForLoadStore(const std::optional<VirtualMemoryAddress>& address, bool store, bool use_fastmem);
  void CompileMoveRegTemplate(Reg dst, Reg src, bool pgxp_move);

  virtual void GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg = Reg::count,
                                            Reg arg3reg = Reg::count) = 0;

  virtual void Compile_Fallback() = 0;

  void Compile_j();
  virtual void Compile_jr(CompileFlags cf) = 0;
  void Compile_jr_const(CompileFlags cf);
  void Compile_jal();
  virtual void Compile_jalr(CompileFlags cf) = 0;
  void Compile_jalr_const(CompileFlags cf);
  void Compile_syscall();
  void Compile_break();

  void Compile_b_const(CompileFlags cf);
  void Compile_b(CompileFlags cf);
  void Compile_blez(CompileFlags cf);
  void Compile_blez_const(CompileFlags cf);
  void Compile_bgtz(CompileFlags cf);
  void Compile_bgtz_const(CompileFlags cf);
  void Compile_beq(CompileFlags cf);
  void Compile_beq_const(CompileFlags cf);
  void Compile_bne(CompileFlags cf);
  void Compile_bne_const(CompileFlags cf);
  virtual void Compile_bxx(CompileFlags cf, BranchCondition cond) = 0;
  void Compile_bxx_const(CompileFlags cf, BranchCondition cond);

  void Compile_sll_const(CompileFlags cf);
  virtual void Compile_sll(CompileFlags cf) = 0;
  void Compile_srl_const(CompileFlags cf);
  virtual void Compile_srl(CompileFlags cf) = 0;
  void Compile_sra_const(CompileFlags cf);
  virtual void Compile_sra(CompileFlags cf) = 0;
  void Compile_sllv_const(CompileFlags cf);
  virtual void Compile_sllv(CompileFlags cf) = 0;
  void Compile_srlv_const(CompileFlags cf);
  virtual void Compile_srlv(CompileFlags cf) = 0;
  void Compile_srav_const(CompileFlags cf);
  virtual void Compile_srav(CompileFlags cf) = 0;
  void Compile_mult_const(CompileFlags cf);
  virtual void Compile_mult(CompileFlags cf) = 0;
  void Compile_multu_const(CompileFlags cf);
  virtual void Compile_multu(CompileFlags cf) = 0;
  void Compile_div_const(CompileFlags cf);
  virtual void Compile_div(CompileFlags cf) = 0;
  void Compile_divu_const(CompileFlags cf);
  virtual void Compile_divu(CompileFlags cf) = 0;
  void Compile_add_const(CompileFlags cf);
  virtual void Compile_add(CompileFlags cf) = 0;
  void Compile_addu_const(CompileFlags cf);
  virtual void Compile_addu(CompileFlags cf) = 0;
  void Compile_sub_const(CompileFlags cf);
  virtual void Compile_sub(CompileFlags cf) = 0;
  void Compile_subu_const(CompileFlags cf);
  virtual void Compile_subu(CompileFlags cf) = 0;
  void Compile_and_const(CompileFlags cf);
  virtual void Compile_and(CompileFlags cf) = 0;
  void Compile_or_const(CompileFlags cf);
  virtual void Compile_or(CompileFlags cf) = 0;
  void Compile_xor_const(CompileFlags cf);
  virtual void Compile_xor(CompileFlags cf) = 0;
  void Compile_nor_const(CompileFlags cf);
  virtual void Compile_nor(CompileFlags cf) = 0;
  void Compile_slt_const(CompileFlags cf);
  virtual void Compile_slt(CompileFlags cf) = 0;
  void Compile_sltu_const(CompileFlags cf);
  virtual void Compile_sltu(CompileFlags cf) = 0;

  void Compile_addi_const(CompileFlags cf);
  virtual void Compile_addi(CompileFlags cf) = 0;
  void Compile_addiu_const(CompileFlags cf);
  virtual void Compile_addiu(CompileFlags cf) = 0;
  void Compile_slti_const(CompileFlags cf);
  virtual void Compile_slti(CompileFlags cf) = 0;
  void Compile_sltiu_const(CompileFlags cf);
  virtual void Compile_sltiu(CompileFlags cf) = 0;
  void Compile_andi_const(CompileFlags cf);
  virtual void Compile_andi(CompileFlags cf) = 0;
  void Compile_ori_const(CompileFlags cf);
  virtual void Compile_ori(CompileFlags cf) = 0;
  void Compile_xori_const(CompileFlags cf);
  virtual void Compile_xori(CompileFlags cf) = 0;
  void Compile_lui();

  virtual void Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                           const std::optional<VirtualMemoryAddress>& address) = 0;
  virtual void Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                           const std::optional<VirtualMemoryAddress>& address) = 0; // lwl/lwr
  virtual void Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                            const std::optional<VirtualMemoryAddress>& address) = 0;
  virtual void Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                           const std::optional<VirtualMemoryAddress>& address) = 0;
  virtual void Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                           const std::optional<VirtualMemoryAddress>& address) = 0; // swl/swr
  virtual void Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                            const std::optional<VirtualMemoryAddress>& address) = 0;

  static u32* GetCop0RegPtr(Cop0Reg reg);
  static u32 GetCop0RegWriteMask(Cop0Reg reg);

  static void MIPSSignedDivide(s32 num, s32 denom, u32* lo, u32* hi);
  static void MIPSUnsignedDivide(u32 num, u32 denom, u32* lo, u32* hi);

  void Compile_mfc0(CompileFlags cf);
  virtual void Compile_mtc0(CompileFlags cf) = 0;
  virtual void Compile_rfe(CompileFlags cf) = 0;

  void AddGTETicks(TickCount ticks);
  void StallUntilGTEComplete();
  virtual void Compile_mfc2(CompileFlags cf) = 0;
  virtual void Compile_mtc2(CompileFlags cf) = 0;
  virtual void Compile_cop2(CompileFlags cf) = 0;

  enum GTERegisterAccessAction : u8
  {
    Ignore,
    Direct,
    ZeroExtend16,
    SignExtend16,
    CallHandler,
    PushFIFO,
  };

  static std::pair<u32*, GTERegisterAccessAction> GetGTERegisterPointer(u32 index, bool writing);

  CodeCache::Block* m_block = nullptr;
  u32 m_compiler_pc = 0;
  TickCount m_cycles = 0;
  TickCount m_gte_done_cycle = 0;

  const Instruction* inst = nullptr;
  CodeCache::InstructionInfo* iinfo = nullptr;
  u32 m_current_instruction_pc = 0;
  bool m_current_instruction_branch_delay_slot = false;
  bool m_branch_delay_slot_swapped = false;

  bool m_dirty_pc = false;
  bool m_dirty_instruction_bits = false;
  bool m_dirty_gte_done_cycle = false;
  bool m_block_ended = false;

  std::bitset<static_cast<size_t>(Reg::count)> m_constant_regs_valid = {};
  std::bitset<static_cast<size_t>(Reg::count)> m_constant_regs_dirty = {};
  std::array<u32, static_cast<size_t>(Reg::count)> m_constant_reg_values = {};

  std::array<HostRegAlloc, NUM_HOST_REGS> m_host_regs = {};
  u16 m_register_alloc_counter = 0;

  bool m_load_delay_dirty = true;
  Reg m_load_delay_register = Reg::count;
  u32 m_load_delay_value_register = 0;

  Reg m_next_load_delay_register = Reg::count;
  u32 m_next_load_delay_value_register = 0;

  struct HostStateBackup
  {
    TickCount cycles;
    TickCount gte_done_cycle;
    u32 compiler_pc;
    bool dirty_pc;
    bool dirty_instruction_bits;
    bool dirty_gte_done_cycle;
    bool block_ended;
    const Instruction* inst;
    CodeCache::InstructionInfo* iinfo;
    u32 current_instruction_pc;
    bool current_instruction_delay_slot;
    std::bitset<static_cast<size_t>(Reg::count)> const_regs_valid;
    std::bitset<static_cast<size_t>(Reg::count)> const_regs_dirty;
    std::array<u32, static_cast<size_t>(Reg::count)> const_regs_values;
    std::array<HostRegAlloc, NUM_HOST_REGS> host_regs;
    u16 register_alloc_counter;
    bool load_delay_dirty;
    Reg load_delay_register;
    u32 load_delay_value_register;
    Reg next_load_delay_register;
    u32 next_load_delay_value_register;
  };

  // we need two of these, one for branch delays, and another if we have an overflow in the delay slot
  std::array<HostStateBackup, 2> m_host_state_backup = {};
  u32 m_host_state_backup_count = 0;

  //////////////////////////////////////////////////////////////////////////
  // Speculative Constants
  //////////////////////////////////////////////////////////////////////////
  using SpecValue = std::optional<u32>;
  struct SpeculativeConstants
  {
    std::array<SpecValue, static_cast<u8>(Reg::count)> regs;
    std::unordered_map<PhysicalMemoryAddress, SpecValue> memory;
    SpecValue cop0_sr;
  };

  void InitSpeculativeRegs();
  void InvalidateSpeculativeValues();
  SpecValue SpecReadReg(Reg reg);
  void SpecWriteReg(Reg reg, SpecValue value);
  void SpecInvalidateReg(Reg reg);
  void SpecCopyReg(Reg dst, Reg src);
  SpecValue SpecReadMem(u32 address);
  void SpecWriteMem(VirtualMemoryAddress address, SpecValue value);
  void SpecInvalidateMem(VirtualMemoryAddress address);
  bool SpecIsCacheIsolated();

  SpeculativeConstants m_speculative_constants;

  void SpecExec_b();
  void SpecExec_jal();
  void SpecExec_jalr();
  void SpecExec_sll();
  void SpecExec_srl();
  void SpecExec_sra();
  void SpecExec_sllv();
  void SpecExec_srlv();
  void SpecExec_srav();
  void SpecExec_mult();
  void SpecExec_multu();
  void SpecExec_div();
  void SpecExec_divu();
  void SpecExec_add();
  void SpecExec_addu();
  void SpecExec_sub();
  void SpecExec_subu();
  void SpecExec_and();
  void SpecExec_or();
  void SpecExec_xor();
  void SpecExec_nor();
  void SpecExec_slt();
  void SpecExec_sltu();
  void SpecExec_addi();
  void SpecExec_addiu();
  void SpecExec_slti();
  void SpecExec_sltiu();
  void SpecExec_andi();
  void SpecExec_ori();
  void SpecExec_xori();
  void SpecExec_lui();
  SpecValue SpecExec_LoadStoreAddr();
  void SpecExec_lxx(MemoryAccessSize size, bool sign);
  void SpecExec_lwx(bool lwr); // lwl/lwr
  void SpecExec_sxx(MemoryAccessSize size);
  void SpecExec_swx(bool swr); // swl/swr
  void SpecExec_swc2();
  void SpecExec_mfc0();
  void SpecExec_mtc0();
  void SpecExec_rfe();

  // PGXP memory callbacks
  static const std::array<std::array<const void*, 2>, 3> s_pgxp_mem_load_functions;
  static const std::array<const void*, 3> s_pgxp_mem_store_functions;
};

void BackpatchLoadStore(void* exception_pc, const CodeCache::LoadstoreBackpatchInfo& info);

u32 CompileLoadStoreThunk(void* thunk_code, u32 thunk_space, void* code_address, u32 code_size, TickCount cycles_to_add,
                          TickCount cycles_to_remove, u32 gpr_bitmask, u8 address_register, u8 data_register,
                          MemoryAccessSize size, bool is_signed, bool is_load);

extern Compiler* g_compiler;
} // namespace CPU::NewRec
