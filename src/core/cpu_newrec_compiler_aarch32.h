// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "cpu_newrec_compiler.h"

#include <memory>

#ifdef CPU_ARCH_ARM32

#include "vixl/aarch32/assembler-aarch32.h"
#include "vixl/aarch32/operands-aarch32.h"

namespace CPU::NewRec {

class AArch32Compiler final : public Compiler
{
public:
  AArch32Compiler();
  ~AArch32Compiler() override;

protected:
  const char* GetHostRegName(u32 reg) const override;

  const void* GetCurrentCodePointer() override;

  void LoadHostRegWithConstant(u32 reg, u32 val) override;
  void LoadHostRegFromCPUPointer(u32 reg, const void* ptr) override;
  void StoreConstantToCPUPointer(u32 val, const void* ptr) override;
  void StoreHostRegToCPUPointer(u32 reg, const void* ptr) override;
  void CopyHostReg(u32 dst, u32 src) override;

  void Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space, u8* far_code_buffer,
             u32 far_code_space) override;
  void BeginBlock() override;
  void GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size) override;
  void GenerateICacheCheckAndUpdate() override;
  void GenerateCall(const void* func, s32 arg1reg = -1, s32 arg2reg = -1, s32 arg3reg = -1) override;
  void EndBlock(const std::optional<u32>& newpc, bool do_event_test) override;
  void EndBlockWithException(Exception excode) override;
  void EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test, bool force_run_events);
  const void* EndCompile(u32* code_size, u32* far_code_size) override;

  void Flush(u32 flags) override;

  void Compile_Fallback() override;

  void CheckBranchTarget(const vixl::aarch32::Register& pcreg);
  void Compile_jr(CompileFlags cf) override;
  void Compile_jalr(CompileFlags cf) override;
  void Compile_bxx(CompileFlags cf, BranchCondition cond) override;

  void Compile_addi(CompileFlags cf, bool overflow);
  void Compile_addi(CompileFlags cf) override;
  void Compile_addiu(CompileFlags cf) override;
  void Compile_slti(CompileFlags cf, bool sign);
  void Compile_slti(CompileFlags cf) override;
  void Compile_sltiu(CompileFlags cf) override;
  void Compile_andi(CompileFlags cf) override;
  void Compile_ori(CompileFlags cf) override;
  void Compile_xori(CompileFlags cf) override;

  void Compile_shift(CompileFlags cf,
                     void (vixl::aarch32::Assembler::*op)(vixl::aarch32::Register, vixl::aarch32::Register,
                                                          const vixl::aarch32::Operand&));
  void Compile_sll(CompileFlags cf) override;
  void Compile_srl(CompileFlags cf) override;
  void Compile_sra(CompileFlags cf) override;
  void Compile_variable_shift(CompileFlags cf,
                              void (vixl::aarch32::Assembler::*op)(vixl::aarch32::Register, vixl::aarch32::Register,
                                                                   const vixl::aarch32::Operand&));
  void Compile_sllv(CompileFlags cf) override;
  void Compile_srlv(CompileFlags cf) override;
  void Compile_srav(CompileFlags cf) override;
  void Compile_mult(CompileFlags cf, bool sign);
  void Compile_mult(CompileFlags cf) override;
  void Compile_multu(CompileFlags cf) override;
  void Compile_div(CompileFlags cf) override;
  void Compile_divu(CompileFlags cf) override;
  void TestOverflow(const vixl::aarch32::Register& result);
  void Compile_dst_op(CompileFlags cf,
                      void (vixl::aarch32::Assembler::*op)(vixl::aarch32::Register, vixl::aarch32::Register,
                                                           const vixl::aarch32::Operand&),
                      bool commutative, bool logical, bool overflow);
  void Compile_add(CompileFlags cf) override;
  void Compile_addu(CompileFlags cf) override;
  void Compile_sub(CompileFlags cf) override;
  void Compile_subu(CompileFlags cf) override;
  void Compile_and(CompileFlags cf) override;
  void Compile_or(CompileFlags cf) override;
  void Compile_xor(CompileFlags cf) override;
  void Compile_nor(CompileFlags cf) override;
  void Compile_slt(CompileFlags cf, bool sign);
  void Compile_slt(CompileFlags cf) override;
  void Compile_sltu(CompileFlags cf) override;

  vixl::aarch32::Register
  ComputeLoadStoreAddressArg(CompileFlags cf, const std::optional<VirtualMemoryAddress>& address,
                             const std::optional<const vixl::aarch32::Register>& reg = std::nullopt);
  template<typename RegAllocFn>
  vixl::aarch32::Register GenerateLoad(const vixl::aarch32::Register& addr_reg, MemoryAccessSize size, bool sign,
                                       bool use_fastmem, const RegAllocFn& dst_reg_alloc);
  void GenerateStore(const vixl::aarch32::Register& addr_reg, const vixl::aarch32::Register& value_reg,
                     MemoryAccessSize size, bool use_fastmem);
  void Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                   const std::optional<VirtualMemoryAddress>& address) override;
  void Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                   const std::optional<VirtualMemoryAddress>& address) override;
  void Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                    const std::optional<VirtualMemoryAddress>& address) override;
  void Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                   const std::optional<VirtualMemoryAddress>& address) override;
  void Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                   const std::optional<VirtualMemoryAddress>& address) override;
  void Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                    const std::optional<VirtualMemoryAddress>& address) override;

  void TestInterrupts(const vixl::aarch32::Register& sr);
  void Compile_mtc0(CompileFlags cf) override;
  void Compile_rfe(CompileFlags cf) override;

  void Compile_mfc2(CompileFlags cf) override;
  void Compile_mtc2(CompileFlags cf) override;
  void Compile_cop2(CompileFlags cf) override;

  void GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg = Reg::count,
                                    Reg arg3reg = Reg::count) override;

private:
  void EmitMov(const vixl::aarch32::Register& dst, u32 val);
  void EmitCall(const void* ptr, bool force_inline = false);

  vixl::aarch32::Operand armCheckAddSubConstant(s32 val);
  vixl::aarch32::Operand armCheckAddSubConstant(u32 val);
  vixl::aarch32::Operand armCheckCompareConstant(s32 val);
  vixl::aarch32::Operand armCheckLogicalConstant(u32 val);

  void SwitchToFarCode(bool emit_jump, vixl::aarch32::ConditionType cond = vixl::aarch32::ConditionType::al);
  void SwitchToFarCodeIfBitSet(const vixl::aarch32::Register& reg, u32 bit);
  void SwitchToFarCodeIfRegZeroOrNonZero(const vixl::aarch32::Register& reg, bool nonzero);
  void SwitchToNearCode(bool emit_jump, vixl::aarch32::ConditionType cond = vixl::aarch32::ConditionType::al);

  void AssertRegOrConstS(CompileFlags cf) const;
  void AssertRegOrConstT(CompileFlags cf) const;
  vixl::aarch32::MemOperand MipsPtr(Reg r) const;
  vixl::aarch32::Register CFGetRegD(CompileFlags cf) const;
  vixl::aarch32::Register CFGetRegS(CompileFlags cf) const;
  vixl::aarch32::Register CFGetRegT(CompileFlags cf) const;
  vixl::aarch32::Register CFGetRegLO(CompileFlags cf) const;
  vixl::aarch32::Register CFGetRegHI(CompileFlags cf) const;
  vixl::aarch32::Register GetMembaseReg();

  void MoveSToReg(const vixl::aarch32::Register& dst, CompileFlags cf);
  void MoveTToReg(const vixl::aarch32::Register& dst, CompileFlags cf);
  void MoveMIPSRegToReg(const vixl::aarch32::Register& dst, Reg reg);

  vixl::aarch32::Assembler m_emitter;
  vixl::aarch32::Assembler m_far_emitter;
  vixl::aarch32::Assembler* armAsm;

#ifdef VIXL_DEBUG
  std::unique_ptr<vixl::CodeBufferCheckScope> m_emitter_check;
  std::unique_ptr<vixl::CodeBufferCheckScope> m_far_emitter_check;
#endif
};

} // namespace CPU::NewRec

#endif // CPU_ARCH_ARM32