#pragma once
#include <array>
#include <initializer_list>
#include <utility>

#include "common/jit_code_buffer.h"

#include "cpu_code_cache.h"
#include "cpu_recompiler_register_cache.h"
#include "cpu_recompiler_thunks.h"
#include "cpu_recompiler_types.h"
#include "cpu_types.h"

// ABI selection
#if defined(Y_CPU_X64)
#if defined(Y_PLATFORM_WINDOWS)
#define ABI_WIN64 1
#elif defined(Y_PLATFORM_LINUX) || defined(Y_PLATFORM_OSX) || defined(Y_PLATFORM_ANDROID)
#define ABI_SYSV 1
#else
#error Unknown ABI.
#endif
#endif

namespace CPU::Recompiler {

class CodeGenerator
{
public:
  CodeGenerator(Core* cpu, JitCodeBuffer* code_buffer, const ASMFunctions& asm_functions);
  ~CodeGenerator();

  static u32 CalculateRegisterOffset(Reg reg);
  static const char* GetHostRegName(HostReg reg, RegSize size = HostPointerSize);
  static void AlignCodeBuffer(JitCodeBuffer* code_buffer);

  bool CompileBlock(const CodeBlock* block, CodeBlock::HostCodePointer* out_host_code, u32* out_host_code_size);

  //////////////////////////////////////////////////////////////////////////
  // Code Generation
  //////////////////////////////////////////////////////////////////////////
  void EmitBeginBlock();
  void EmitEndBlock();
  void EmitExceptionExit();
  void EmitExceptionExitOnBool(const Value& value);
  void FinalizeBlock(CodeBlock::HostCodePointer* out_host_code, u32* out_host_code_size);

  void EmitSignExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size);
  void EmitZeroExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size);
  void EmitCopyValue(HostReg to_reg, const Value& value);
  void EmitAdd(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags);
  void EmitSub(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags);
  void EmitCmp(HostReg to_reg, const Value& value);
  void EmitMul(HostReg to_reg_hi, HostReg to_reg_lo, const Value& lhs, const Value& rhs, bool signed_multiply);
  void EmitInc(HostReg to_reg, RegSize size);
  void EmitDec(HostReg to_reg, RegSize size);
  void EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value);
  void EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value);
  void EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value);
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
  void EmitLoadCPUStructField(HostReg host_reg, RegSize size, u32 offset);
  void EmitStoreCPUStructField(u32 offset, const Value& value);
  void EmitAddCPUStructField(u32 offset, const Value& value);

  // Automatically generates an exception handler.
  Value EmitLoadGuestMemory(const Value& address, RegSize size);
  void EmitStoreGuestMemory(const Value& address, const Value& value);

  // Branching, generates two paths.
  void EmitBranch(Condition condition, Reg lr_reg, Value&& branch_target);
  void EmitBranchIfBitClear(HostReg reg, RegSize size, u8 bit, LabelType* label);
  void EmitBindLabel(LabelType* label);

  // Raising exception if condition is true.
  void EmitRaiseException(Exception excode, Condition condition = Condition::Always);

  u32 PrepareStackForCall();
  void RestoreStackAfterCall(u32 adjust_size);

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
  void EmitPopHostReg(HostReg reg, u32 position);

  // Value ops
  Value AddValues(const Value& lhs, const Value& rhs, bool set_flags);
  Value SubValues(const Value& lhs, const Value& rhs, bool set_flags);
  std::pair<Value, Value> MulValues(const Value& lhs, const Value& rhs, bool signed_multiply);
  Value ShlValues(const Value& lhs, const Value& rhs);
  Value ShrValues(const Value& lhs, const Value& rhs);
  Value SarValues(const Value& lhs, const Value& rhs);
  Value OrValues(const Value& lhs, const Value& rhs);
  Value AndValues(const Value& lhs, const Value& rhs);
  Value XorValues(const Value& lhs, const Value& rhs);
  Value NotValue(const Value& val);

private:
  // Host register setup
  void InitHostRegs();

  Value ConvertValueSize(const Value& value, RegSize size, bool sign_extend);
  void ConvertValueSizeInPlace(Value* value, RegSize size, bool sign_extend);

  Value GetValueInHostRegister(const Value& value);

  void SwitchToFarCode();
  void SwitchToNearCode();
  void* GetCurrentNearCodePointer() const;
  void* GetCurrentFarCodePointer() const;

  //////////////////////////////////////////////////////////////////////////
  // Code Generation Helpers
  //////////////////////////////////////////////////////////////////////////
  // branch target, memory address, etc
  void BlockPrologue();
  void BlockEpilogue();
  void InstructionPrologue(const CodeBlockInstruction& cbi, TickCount cycles, bool force_sync = false);
  void InstructionEpilogue(const CodeBlockInstruction& cbi);
  void AddPendingCycles();

  Value DoGTERegisterRead(u32 index);
  void DoGTERegisterWrite(u32 index, const Value& value);

  //////////////////////////////////////////////////////////////////////////
  // Instruction Code Generators
  //////////////////////////////////////////////////////////////////////////
  bool CompileInstruction(const CodeBlockInstruction& cbi);
  bool Compile_Fallback(const CodeBlockInstruction& cbi);
  bool Compile_Bitwise(const CodeBlockInstruction& cbi);
  bool Compile_Shift(const CodeBlockInstruction& cbi);
  bool Compile_Load(const CodeBlockInstruction& cbi);
  bool Compile_Store(const CodeBlockInstruction& cbi);
  bool Compile_MoveHiLo(const CodeBlockInstruction& cbi);
  bool Compile_Add(const CodeBlockInstruction& cbi);
  bool Compile_Subtract(const CodeBlockInstruction& cbi);
  bool Compile_Multiply(const CodeBlockInstruction& cbi);
  bool Compile_SetLess(const CodeBlockInstruction& cbi);
  bool Compile_Branch(const CodeBlockInstruction& cbi);
  bool Compile_lui(const CodeBlockInstruction& cbi);
  bool Compile_cop0(const CodeBlockInstruction& cbi);
  bool Compile_cop2(const CodeBlockInstruction& cbi);

  Core* m_cpu;
  JitCodeBuffer* m_code_buffer;
  const ASMFunctions& m_asm_functions;
  const CodeBlock* m_block = nullptr;
  const CodeBlockInstruction* m_block_start = nullptr;
  const CodeBlockInstruction* m_block_end = nullptr;
  RegisterCache m_register_cache;
  CodeEmitter m_near_emitter;
  CodeEmitter m_far_emitter;
  CodeEmitter* m_emit;

  TickCount m_delayed_cycles_add = 0;

  // whether various flags need to be reset.
  bool m_current_instruction_in_branch_delay_slot_dirty = false;
  bool m_branch_was_taken_dirty = false;
  bool m_current_instruction_was_branch_taken_dirty = false;
  bool m_load_delay_dirty = false;
  bool m_next_load_delay_dirty = false;
};

} // namespace CPU::Recompiler
