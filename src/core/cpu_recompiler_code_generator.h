// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "cpu_code_cache_private.h"
#include "cpu_recompiler_register_cache.h"
#include "cpu_recompiler_thunks.h"
#include "cpu_recompiler_types.h"
#include "cpu_types.h"

#include <array>
#include <utility>

namespace CPU::Recompiler {

enum class Condition : u8
{
  Always,
  NotEqual,
  Equal,
  Overflow,
  Greater,
  GreaterEqual,
  LessEqual,
  Less,
  Negative,
  PositiveOrZero,
  Above,      // unsigned variant of Greater
  AboveEqual, // unsigned variant of GreaterEqual
  Below,      // unsigned variant of Less
  BelowEqual, // unsigned variant of LessEqual

  NotZero,
  Zero
};

class CodeGenerator
{
public:
  using SpeculativeValue = std::optional<u32>;

  struct CodeBlockInstruction
  {
    const Instruction* instruction;
    const CodeCache::InstructionInfo* info;
  };

  CodeGenerator();
  ~CodeGenerator();

  static const char* GetHostRegName(HostReg reg, RegSize size = HostPointerSize);

  static void BackpatchLoadStore(void* host_pc, const CodeCache::LoadstoreBackpatchInfo& lbi);

  const void* CompileBlock(CodeCache::Block* block, u32* out_host_code_size, u32* out_host_far_code_size);

  //////////////////////////////////////////////////////////////////////////
  // Code Generation
  //////////////////////////////////////////////////////////////////////////
  void EmitBeginBlock(bool allocate_registers = true);
  void EmitEndBlock(bool free_registers, const void* jump_to);
  void EmitExceptionExit();
  void EmitExceptionExitOnBool(const Value& value);
  const void* FinalizeBlock(u32* out_host_code_size, u32* out_host_far_code_size);

  void EmitSignExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size);
  void EmitZeroExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size);
  void EmitCopyValue(HostReg to_reg, const Value& value);
  void EmitAdd(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags);
  void EmitSub(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags);
  void EmitCmp(HostReg to_reg, const Value& value);
  void EmitMul(HostReg to_reg_hi, HostReg to_reg_lo, const Value& lhs, const Value& rhs, bool signed_multiply);
  void EmitDiv(HostReg to_reg_quotient, HostReg to_reg_remainder, HostReg num, HostReg denom, RegSize size,
               bool signed_divide);
  void EmitInc(HostReg to_reg, RegSize size);
  void EmitDec(HostReg to_reg, RegSize size);
  void EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
               bool assume_amount_masked = true);
  void EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
               bool assume_amount_masked = true);
  void EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
               bool assume_amount_masked = true);
  void EmitAnd(HostReg to_reg, HostReg from_reg, const Value& value);
  void EmitOr(HostReg to_reg, HostReg from_reg, const Value& value);
  void EmitXor(HostReg to_reg, HostReg from_reg, const Value& value);
  void EmitTest(HostReg to_reg, const Value& value);
  void EmitNot(HostReg to_reg, RegSize size);
  void EmitSetConditionResult(HostReg to_reg, RegSize to_size, Condition condition);

  void EmitLoadGuestRegister(HostReg host_reg, Reg guest_reg);
  void EmitStoreGuestRegister(Reg guest_reg, const Value& value);
  void EmitStoreInterpreterLoadDelay(Reg reg, const Value& value);
  void EmitFlushInterpreterLoadDelay();
  void EmitMoveNextInterpreterLoadDelay();
  void EmitCancelInterpreterLoadDelayForReg(Reg reg);
  void EmitICacheCheckAndUpdate();
  void EmitBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size);
  void EmitStallUntilGTEComplete();
  void EmitLoadCPUStructField(HostReg host_reg, RegSize size, u32 offset);
  void EmitStoreCPUStructField(u32 offset, const Value& value);
  void EmitAddCPUStructField(u32 offset, const Value& value);
  void EmitLoadGlobal(HostReg host_reg, RegSize size, const void* ptr);
  void EmitStoreGlobal(void* ptr, const Value& value);
  void EmitLoadGlobalAddress(HostReg host_reg, const void* ptr);

  // Automatically generates an exception handler.
  Value EmitLoadGuestMemory(Instruction instruction, const CodeCache::InstructionInfo& info, const Value& address,
                            const SpeculativeValue& address_spec, RegSize size);
  void EmitLoadGuestRAMFastmem(const Value& address, RegSize size, Value& result);
  void EmitLoadGuestMemoryFastmem(Instruction instruction, const CodeCache::InstructionInfo& info, const Value& address,
                                  RegSize size, Value& result);
  void EmitLoadGuestMemorySlowmem(Instruction instruction, const CodeCache::InstructionInfo& info, const Value& address,
                                  RegSize size, Value& result, bool in_far_code);
  void EmitStoreGuestMemory(Instruction instruction, const CodeCache::InstructionInfo& info, const Value& address,
                            const SpeculativeValue& address_spec, RegSize size, const Value& value);
  void EmitStoreGuestMemoryFastmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                   const Value& address, RegSize size, const Value& value);
  void EmitStoreGuestMemorySlowmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                   const Value& address, RegSize size, const Value& value, bool in_far_code);
  void EnsureMembaseLoaded();
  void EmitUpdateFastmemBase();

  // Unconditional branch to pointer. May allocate a scratch register.
  void EmitBranch(const void* address, bool allow_scratch = true);
  void EmitBranch(LabelType* label);

  // Branching, generates two paths.
  void EmitConditionalBranch(Condition condition, bool invert, HostReg value, RegSize size, LabelType* label);
  void EmitConditionalBranch(Condition condition, bool invert, HostReg lhs, const Value& rhs, LabelType* label);
  void EmitConditionalBranch(Condition condition, bool invert, LabelType* label);
  void EmitBranchIfBitClear(HostReg reg, RegSize size, u8 bit, LabelType* label);
  void EmitBranchIfBitSet(HostReg reg, RegSize size, u8 bit, LabelType* label);
  void EmitBindLabel(LabelType* label);

  u32 PrepareStackForCall();
  void RestoreStackAfterCall(u32 adjust_size);

  void EmitCall(const void* ptr);
  void EmitFunctionCallPtr(Value* return_value, const void* ptr);
  void EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1);
  void EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2);
  void EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2,
                           const Value& arg3);
  void EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2,
                           const Value& arg3, const Value& arg4);

  template<typename FunctionType>
  void EmitFunctionCall(Value* return_value, const FunctionType ptr)
  {
    EmitFunctionCallPtr(return_value, reinterpret_cast<const void**>(ptr));
  }

  template<typename FunctionType>
  void EmitFunctionCall(Value* return_value, const FunctionType ptr, const Value& arg1)
  {
    EmitFunctionCallPtr(return_value, reinterpret_cast<const void**>(ptr), arg1);
  }

  template<typename FunctionType>
  void EmitFunctionCall(Value* return_value, const FunctionType ptr, const Value& arg1, const Value& arg2)
  {
    EmitFunctionCallPtr(return_value, reinterpret_cast<const void**>(ptr), arg1, arg2);
  }

  template<typename FunctionType>
  void EmitFunctionCall(Value* return_value, const FunctionType ptr, const Value& arg1, const Value& arg2,
                        const Value& arg3)
  {
    EmitFunctionCallPtr(return_value, reinterpret_cast<const void**>(ptr), arg1, arg2, arg3);
  }

  template<typename FunctionType>
  void EmitFunctionCall(Value* return_value, const FunctionType ptr, const Value& arg1, const Value& arg2,
                        const Value& arg3, const Value& arg4)
  {
    EmitFunctionCallPtr(return_value, reinterpret_cast<const void**>(ptr), arg1, arg2, arg3, arg4);
  }

  // Host register saving.
  void EmitPushHostReg(HostReg reg, u32 position);
  void EmitPushHostRegPair(HostReg reg, HostReg reg2, u32 position);
  void EmitPopHostReg(HostReg reg, u32 position);
  void EmitPopHostRegPair(HostReg reg, HostReg reg2, u32 position);

  // Value ops
  Value AddValues(const Value& lhs, const Value& rhs, bool set_flags);
  Value SubValues(const Value& lhs, const Value& rhs, bool set_flags);
  std::pair<Value, Value> MulValues(const Value& lhs, const Value& rhs, bool signed_multiply);
  Value ShlValues(const Value& lhs, const Value& rhs, bool assume_amount_masked = true);
  Value ShrValues(const Value& lhs, const Value& rhs, bool assume_amount_masked = true);
  Value SarValues(const Value& lhs, const Value& rhs, bool assume_amount_masked = true);
  Value OrValues(const Value& lhs, const Value& rhs);
  void OrValueInPlace(Value& lhs, const Value& rhs);
  Value AndValues(const Value& lhs, const Value& rhs);
  void AndValueInPlace(Value& lhs, const Value& rhs);
  Value XorValues(const Value& lhs, const Value& rhs);
  Value NotValue(const Value& val);

  const TickCount* GetFetchMemoryAccessTimePtr() const;

  // Raising exception if condition is true.
  void GenerateExceptionExit(Instruction instruction, const CodeCache::InstructionInfo& info, Exception excode,
                             Condition condition = Condition::Always);

private:
  // Host register setup
  void InitHostRegs();

  Value ConvertValueSize(const Value& value, RegSize size, bool sign_extend);
  void ConvertValueSizeInPlace(Value* value, RegSize size, bool sign_extend);

  Value GetValueInHostRegister(const Value& value, bool allow_zero_register = true);
  Value GetValueInHostOrScratchRegister(const Value& value, bool allow_zero_register = true);

  void SwitchToFarCode();
  void SwitchToNearCode();
  void* GetStartNearCodePointer() const;
  void* GetCurrentCodePointer() const;
  void* GetCurrentNearCodePointer() const;
  void* GetCurrentFarCodePointer() const;

  //////////////////////////////////////////////////////////////////////////
  // Code Generation Helpers
  //////////////////////////////////////////////////////////////////////////
  // branch target, memory address, etc
  void BlockPrologue();
  void BlockEpilogue();
  void InstructionPrologue(Instruction instruction, const CodeCache::InstructionInfo& info, TickCount cycles,
                           bool force_sync = false);
  void InstructionEpilogue(Instruction instruction, const CodeCache::InstructionInfo& info);
  void TruncateBlockAtCurrentInstruction();
  void AddPendingCycles(bool commit);
  void AddGTETicks(TickCount ticks);
  void StallUntilGTEComplete();

  Value CalculatePC(u32 offset = 0);
  Value GetCurrentInstructionPC(u32 offset = 0);
  void WriteNewPC(const Value& value, bool commit);

  Value DoGTERegisterRead(u32 index);
  void DoGTERegisterWrite(u32 index, const Value& value);

  //////////////////////////////////////////////////////////////////////////
  // Instruction Code Generators
  //////////////////////////////////////////////////////////////////////////
  bool CompileInstruction(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Fallback(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Nop(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Bitwise(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Shift(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Load(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Store(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_LoadLeftRight(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_StoreLeftRight(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_MoveHiLo(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Add(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Subtract(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Multiply(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Divide(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_SignedDivide(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_SetLess(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_Branch(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_lui(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_cop0(Instruction instruction, const CodeCache::InstructionInfo& info);
  bool Compile_cop2(Instruction instruction, const CodeCache::InstructionInfo& info);

  CodeCache::Block* m_block = nullptr;
  CodeBlockInstruction m_block_start = {};
  CodeBlockInstruction m_block_end = {};
  CodeBlockInstruction m_current_instruction = {};
  RegisterCache m_register_cache;
  CodeEmitter m_near_emitter;
  CodeEmitter m_far_emitter;
  CodeEmitter* m_emit;

  TickCount m_delayed_cycles_add = 0;
  TickCount m_gte_done_cycle = 0;

  u32 m_pc = 0;
  bool m_pc_valid = false;
  bool m_block_linked = false;

  // whether various flags need to be reset.
  bool m_current_instruction_in_branch_delay_slot_dirty = false;
  bool m_branch_was_taken_dirty = false;
  bool m_current_instruction_was_branch_taken_dirty = false;
  bool m_load_delay_dirty = false;
  bool m_next_load_delay_dirty = false;
  bool m_gte_busy_cycles_dirty = false;
  bool m_membase_loaded = false;

  //////////////////////////////////////////////////////////////////////////
  // Speculative Constants
  //////////////////////////////////////////////////////////////////////////
  struct SpeculativeConstants
  {
    std::array<SpeculativeValue, static_cast<u8>(Reg::count)> regs;
    std::unordered_map<PhysicalMemoryAddress, SpeculativeValue> memory;
    SpeculativeValue cop0_sr;
  };

  void InitSpeculativeRegs();
  void InvalidateSpeculativeValues();
  SpeculativeValue SpeculativeReadReg(Reg reg);
  void SpeculativeWriteReg(Reg reg, SpeculativeValue value);
  SpeculativeValue SpeculativeReadMemory(u32 address);
  void SpeculativeWriteMemory(VirtualMemoryAddress address, SpeculativeValue value);
  bool SpeculativeIsCacheIsolated();

  SpeculativeConstants m_speculative_constants;
};

} // namespace CPU::Recompiler
