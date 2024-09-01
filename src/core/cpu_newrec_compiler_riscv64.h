// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "cpu_newrec_compiler.h"

#include <memory>

#ifdef CPU_ARCH_RISCV64

namespace CPU::NewRec {

class RISCV64Compiler final : public Compiler
{
public:
  RISCV64Compiler();
  ~RISCV64Compiler() override;

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
  void GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size) override;
  void GenerateICacheCheckAndUpdate() override;
  void GenerateCall(const void* func, s32 arg1reg = -1, s32 arg2reg = -1, s32 arg3reg = -1) override;
  void EndBlock(const std::optional<u32>& newpc, bool do_event_test) override;
  void EndBlockWithException(Exception excode) override;
  void EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test, bool force_run_events);
  const void* EndCompile(u32* code_size, u32* far_code_size) override;

  void Flush(u32 flags) override;

  void Compile_Fallback() override;

  void CheckBranchTarget(const biscuit::GPR& pcreg);
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

  void Compile_shift(CompileFlags cf, void (biscuit::Assembler::*op)(biscuit::GPR, biscuit::GPR, biscuit::GPR),
                     void (biscuit::Assembler::*op_const)(biscuit::GPR, biscuit::GPR, unsigned));
  void Compile_sll(CompileFlags cf) override;
  void Compile_srl(CompileFlags cf) override;
  void Compile_sra(CompileFlags cf) override;
  void Compile_variable_shift(CompileFlags cf, void (biscuit::Assembler::*op)(biscuit::GPR, biscuit::GPR, biscuit::GPR),
                              void (biscuit::Assembler::*op_const)(biscuit::GPR, biscuit::GPR, unsigned));
  void Compile_sllv(CompileFlags cf) override;
  void Compile_srlv(CompileFlags cf) override;
  void Compile_srav(CompileFlags cf) override;
  void Compile_mult(CompileFlags cf, bool sign);
  void Compile_mult(CompileFlags cf) override;
  void Compile_multu(CompileFlags cf) override;
  void Compile_div(CompileFlags cf) override;
  void Compile_divu(CompileFlags cf) override;
  void TestOverflow(const biscuit::GPR& long_res, const biscuit::GPR& res, const biscuit::GPR& reg_to_discard);
  void Compile_dst_op(CompileFlags cf, void (biscuit::Assembler::*op)(biscuit::GPR, biscuit::GPR, biscuit::GPR),
                      void (RISCV64Compiler::*op_const)(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm),
                      void (biscuit::Assembler::*op_long)(biscuit::GPR, biscuit::GPR, biscuit::GPR), bool commutative,
                      bool overflow);
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

  biscuit::GPR ComputeLoadStoreAddressArg(CompileFlags cf, const std::optional<VirtualMemoryAddress>& address,
                                          const std::optional<const biscuit::GPR>& reg = std::nullopt);
  template<typename RegAllocFn>
  biscuit::GPR GenerateLoad(const biscuit::GPR& addr_reg, MemoryAccessSize size, bool sign, bool use_fastmem,
                            const RegAllocFn& dst_reg_alloc);
  void GenerateStore(const biscuit::GPR& addr_reg, const biscuit::GPR& value_reg, MemoryAccessSize size,
                     bool use_fastmem);
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

  void TestInterrupts(const biscuit::GPR& sr);
  void Compile_mtc0(CompileFlags cf) override;
  void Compile_rfe(CompileFlags cf) override;

  void Compile_mfc2(CompileFlags cf) override;
  void Compile_mtc2(CompileFlags cf) override;
  void Compile_cop2(CompileFlags cf) override;

  void GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg = Reg::count,
                                    Reg arg3reg = Reg::count) override;

private:
  void EmitMov(const biscuit::GPR& dst, u32 val);
  void EmitCall(const void* ptr);

  void SwitchToFarCode(bool emit_jump,
                       void (biscuit::Assembler::*inverted_cond)(biscuit::GPR, biscuit::GPR, biscuit::Label*) = nullptr,
                       const biscuit::GPR& rs1 = biscuit::zero, const biscuit::GPR& rs2 = biscuit::zero);
  void SwitchToNearCode(bool emit_jump);

  void AssertRegOrConstS(CompileFlags cf) const;
  void AssertRegOrConstT(CompileFlags cf) const;
  // vixl::aarch64::MemOperand MipsPtr(Reg r) const;

  void SafeImmSExtIType(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm,
                        void (biscuit::Assembler::*iop)(biscuit::GPR, biscuit::GPR, u32),
                        void (biscuit::Assembler::*rop)(biscuit::GPR, biscuit::GPR, biscuit::GPR));

  void SafeADDI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeADDIW(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeSUBIW(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeANDI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeORI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeXORI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeSLTI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);
  void SafeSLTIU(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm);

  void EmitSExtB(const biscuit::GPR& rd, const biscuit::GPR& rs);
  void EmitUExtB(const biscuit::GPR& rd, const biscuit::GPR& rs);
  void EmitSExtH(const biscuit::GPR& rd, const biscuit::GPR& rs);
  void EmitUExtH(const biscuit::GPR& rd, const biscuit::GPR& rs);
  void EmitDSExtW(const biscuit::GPR& rd, const biscuit::GPR& rs);
  void EmitDUExtW(const biscuit::GPR& rd, const biscuit::GPR& rs);

  biscuit::GPR CFGetSafeRegS(CompileFlags cf, const biscuit::GPR& temp_reg);
  biscuit::GPR CFGetSafeRegT(CompileFlags cf, const biscuit::GPR& temp_reg);

  biscuit::GPR CFGetRegD(CompileFlags cf) const;
  biscuit::GPR CFGetRegS(CompileFlags cf) const;
  biscuit::GPR CFGetRegT(CompileFlags cf) const;
  biscuit::GPR CFGetRegLO(CompileFlags cf) const;
  biscuit::GPR CFGetRegHI(CompileFlags cf) const;

  void MoveSToReg(const biscuit::GPR& dst, CompileFlags cf);
  void MoveTToReg(const biscuit::GPR& dst, CompileFlags cf);
  void MoveMIPSRegToReg(const biscuit::GPR& dst, Reg reg);

  std::unique_ptr<biscuit::Assembler> m_emitter;
  std::unique_ptr<biscuit::Assembler> m_far_emitter;
  biscuit::Assembler* rvAsm;
};

} // namespace CPU::NewRec

#endif // CPU_ARCH_RISCV64
