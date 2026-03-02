// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "cpu_recompiler.h"

#ifdef CPU_ARCH_LOONGARCH64

extern "C" {
#include "lagoon.h"
}

namespace CPU {

// Function pointer types for lagoon operations
using LaRRROp = void (*)(lagoon_assembler_t*, la_gpr_t, la_gpr_t, la_gpr_t);
using LaRROp = void (*)(lagoon_assembler_t*, la_gpr_t, la_gpr_t);
using LaRRSImmOp = void (*)(lagoon_assembler_t*, la_gpr_t, la_gpr_t, int32_t);
using LaRRUImmOp = void (*)(lagoon_assembler_t*, la_gpr_t, la_gpr_t, uint32_t);

// Branch condition enum for SwitchToFarCode
enum class LaBranchCondition
{
  None,
  EQ,
  NE,
  LT,
  GE,
  LTU,
  GEU
};

class LoongArch64Recompiler final : public Recompiler
{
public:
  LoongArch64Recompiler();
  ~LoongArch64Recompiler() override;

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

  void CheckBranchTarget(la_gpr_t pcreg);
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

  void Compile_shift(CompileFlags cf, LaRRROp op, LaRRUImmOp op_const);
  void Compile_sll(CompileFlags cf) override;
  void Compile_srl(CompileFlags cf) override;
  void Compile_sra(CompileFlags cf) override;
  void Compile_variable_shift(CompileFlags cf, LaRRROp op, LaRRUImmOp op_const);
  void Compile_sllv(CompileFlags cf) override;
  void Compile_srlv(CompileFlags cf) override;
  void Compile_srav(CompileFlags cf) override;
  void Compile_mult(CompileFlags cf, bool sign);
  void Compile_mult(CompileFlags cf) override;
  void Compile_multu(CompileFlags cf) override;
  void Compile_div(CompileFlags cf) override;
  void Compile_divu(CompileFlags cf) override;
  void TestOverflow(la_gpr_t long_res, la_gpr_t res, la_gpr_t reg_to_discard);
  void Compile_dst_op(CompileFlags cf, LaRRROp op,
                      void (LoongArch64Recompiler::*op_const)(la_gpr_t rd, la_gpr_t rs, u32 imm), LaRRROp op_long,
                      bool commutative, bool overflow);
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

  la_gpr_t ComputeLoadStoreAddressArg(CompileFlags cf, const std::optional<VirtualMemoryAddress>& address,
                                      const std::optional<la_gpr_t>& reg = std::nullopt);
  template<typename RegAllocFn>
  la_gpr_t GenerateLoad(la_gpr_t addr_reg, MemoryAccessSize size, bool sign, bool use_fastmem,
                        const RegAllocFn& dst_reg_alloc);
  void GenerateStore(la_gpr_t addr_reg, la_gpr_t value_reg, MemoryAccessSize size, bool use_fastmem);
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

  void TestInterrupts(la_gpr_t sr);
  void Compile_mtc0(CompileFlags cf) override;
  void Compile_rfe(CompileFlags cf) override;

  void Compile_mfc2(CompileFlags cf) override;
  void Compile_mtc2(CompileFlags cf) override;
  void Compile_cop2(CompileFlags cf) override;

  void GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg = Reg::count,
                                    Reg arg3reg = Reg::count) override;

private:
  void EmitMov(la_gpr_t dst, u32 val);
  void EmitCall(const void* ptr);

  void SwitchToFarCode(bool emit_jump, LaBranchCondition cond = LaBranchCondition::None, la_gpr_t rs1 = LA_ZERO,
                       la_gpr_t rs2 = LA_ZERO);
  void SwitchToNearCode(bool emit_jump);

  void AssertRegOrConstS(CompileFlags cf) const;
  void AssertRegOrConstT(CompileFlags cf) const;

  void SafeImmSImm12(la_gpr_t rd, la_gpr_t rs, u32 imm, LaRRSImmOp iop, LaRRROp rop);
  void SafeImmUImm12(la_gpr_t rd, la_gpr_t rs, u32 imm, LaRRUImmOp iop, LaRRROp rop);

  void SafeADDI(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeADDIW(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeSUBIW(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeANDI(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeORI(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeXORI(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeSLTI(la_gpr_t rd, la_gpr_t rs, u32 imm);
  void SafeSLTIU(la_gpr_t rd, la_gpr_t rs, u32 imm);

  void EmitSExtB(la_gpr_t rd, la_gpr_t rs);
  void EmitUExtB(la_gpr_t rd, la_gpr_t rs);
  void EmitSExtH(la_gpr_t rd, la_gpr_t rs);
  void EmitUExtH(la_gpr_t rd, la_gpr_t rs);
  void EmitDSExtW(la_gpr_t rd, la_gpr_t rs);
  void EmitDUExtW(la_gpr_t rd, la_gpr_t rs);

  la_gpr_t CFGetSafeRegS(CompileFlags cf, la_gpr_t temp_reg);
  la_gpr_t CFGetSafeRegT(CompileFlags cf, la_gpr_t temp_reg);

  la_gpr_t CFGetRegD(CompileFlags cf) const;
  la_gpr_t CFGetRegS(CompileFlags cf) const;
  la_gpr_t CFGetRegT(CompileFlags cf) const;
  la_gpr_t CFGetRegLO(CompileFlags cf) const;
  la_gpr_t CFGetRegHI(CompileFlags cf) const;

  void MoveSToReg(la_gpr_t dst, CompileFlags cf);
  void MoveTToReg(la_gpr_t dst, CompileFlags cf);
  void MoveMIPSRegToReg(la_gpr_t dst, Reg reg, bool ignore_load_delays = false);

  lagoon_assembler_t m_emitter;
  lagoon_assembler_t m_far_emitter;
  lagoon_assembler_t* laAsm;
};

} // namespace CPU

#endif // CPU_ARCH_LOONGARCH64