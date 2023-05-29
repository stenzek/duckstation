#include <biscuit/assert.hpp>
#include <biscuit/assembler.hpp>

namespace biscuit {
namespace {
void EmitAES32Instruction(CodeBuffer& buffer, uint32_t op, GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    BISCUIT_ASSERT(bs <= 0b11);
    buffer.Emit32(op | (bs << 30) | (rs2.Index() << 20) |
                  (rs1.Index() << 15) | (rd.Index() << 7));
}

void EmitSM4Instruction(CodeBuffer& buffer, uint32_t op, GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    // Same behavior, function exists for a better contextual name.
    EmitAES32Instruction(buffer, op, rd, rs1, rs2, bs);
}

void EmitAES64Instruction(CodeBuffer& buffer, uint32_t op, GPR rd, GPR rs1, GPR rs2) noexcept {
    buffer.Emit32(op | (rs2.Index() << 20) | (rs1.Index() << 15) | (rd.Index() << 7));
}

void EmitSHAInstruction(CodeBuffer& buffer, uint32_t op, GPR rd, GPR rs1, GPR rs2) noexcept {
    // Same behavior, function exists for a better contextual name.
    EmitAES64Instruction(buffer, op, rd, rs1, rs2);
}

void EmitSM3Instruction(CodeBuffer& buffer, uint32_t op, GPR rd, GPR rs) noexcept {
    // Same behavior, function exists for a better contextual name.
    EmitAES64Instruction(buffer, op, rd, rs, x0);
}
} // Anonymous namespace

void Assembler::AES32DSI(GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    EmitAES32Instruction(m_buffer, 0x2A000033, rd, rs1, rs2, bs);
}

void Assembler::AES32DSMI(GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    EmitAES32Instruction(m_buffer, 0x2E000033, rd, rs1, rs2, bs);
}

void Assembler::AES32ESI(GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    EmitAES32Instruction(m_buffer, 0x22000033, rd, rs1, rs2, bs);
}

void Assembler::AES32ESMI(GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    EmitAES32Instruction(m_buffer, 0x26000033, rd, rs1, rs2, bs);
}

void Assembler::AES64DS(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitAES64Instruction(m_buffer, 0x3A000033, rd, rs1, rs2);
}

void Assembler::AES64DSM(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitAES64Instruction(m_buffer, 0x3E000033, rd, rs1, rs2);
}

void Assembler::AES64ES(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitAES64Instruction(m_buffer, 0x32000033, rd, rs1, rs2);
}

void Assembler::AES64ESM(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitAES64Instruction(m_buffer, 0x36000033, rd, rs1, rs2);
}

void Assembler::AES64IM(GPR rd, GPR rs) noexcept {
    EmitAES64Instruction(m_buffer, 0x30001013, rd, rs, x0);
}

void Assembler::AES64KS1I(GPR rd, GPR rs, uint32_t rnum) noexcept {
    // RVK spec states that 0xB to 0xF are reserved.
    BISCUIT_ASSERT(rnum <= 0xA);
    EmitAES64Instruction(m_buffer, 0x31001013, rd, rs, GPR{rnum});
}

void Assembler::AES64KS2(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitAES64Instruction(m_buffer, 0x7E000033, rd, rs1, rs2);
}

void Assembler::SHA256SIG0(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10201013, rd, rs, x0);
}

void Assembler::SHA256SIG1(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10301013, rd, rs, x0);
}

void Assembler::SHA256SUM0(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10001013, rd, rs, x0);
}

void Assembler::SHA256SUM1(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10101013, rd, rs, x0);
}

void Assembler::SHA512SIG0(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10601013, rd, rs, x0);
}

void Assembler::SHA512SIG0H(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitSHAInstruction(m_buffer, 0x5C000033, rd, rs1, rs2);
}

void Assembler::SHA512SIG0L(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitSHAInstruction(m_buffer, 0x54000033, rd, rs1, rs2);
}

void Assembler::SHA512SIG1(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10701013, rd, rs, x0);
}

void Assembler::SHA512SIG1H(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitSHAInstruction(m_buffer, 0x5E000033, rd, rs1, rs2);
}

void Assembler::SHA512SIG1L(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitSHAInstruction(m_buffer, 0x56000033, rd, rs1, rs2);
}

void Assembler::SHA512SUM0(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10401013, rd, rs, x0);
}

void Assembler::SHA512SUM0R(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitSHAInstruction(m_buffer, 0x50000033, rd, rs1, rs2);
}

void Assembler::SHA512SUM1(GPR rd, GPR rs) noexcept {
    EmitSHAInstruction(m_buffer, 0x10501013, rd, rs, x0);
}

void Assembler::SHA512SUM1R(GPR rd, GPR rs1, GPR rs2) noexcept {
    EmitSHAInstruction(m_buffer, 0x52000033, rd, rs1, rs2);
}

void Assembler::SM3P0(GPR rd, GPR rs) noexcept {
    EmitSM3Instruction(m_buffer, 0x10801013, rd, rs);
}

void Assembler::SM3P1(GPR rd, GPR rs) noexcept {
    EmitSM3Instruction(m_buffer, 0x10901013, rd, rs);
}

void Assembler::SM4ED(GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    EmitSM4Instruction(m_buffer, 0x30000033, rd, rs1, rs2, bs);
}

void Assembler::SM4KS(GPR rd, GPR rs1, GPR rs2, uint32_t bs) noexcept {
    EmitSM4Instruction(m_buffer, 0x34000033, rd, rs1, rs2, bs);
}
} // namespace biscuit
