#ifndef LAGOON_H
#define LAGOON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LA_ZERO = 0,
    LA_RA,
    LA_TP,
    LA_SP,
    LA_A0,
    LA_A1,
    LA_A2,
    LA_A3,
    LA_A4,
    LA_A5,
    LA_A6,
    LA_A7,
    LA_T0,
    LA_T1,
    LA_T2,
    LA_T3,
    LA_T4,
    LA_T5,
    LA_T6,
    LA_T7,
    LA_T8,
    LA_RX,
    LA_FP,
    LA_S0,
    LA_S1,
    LA_S2,
    LA_S3,
    LA_S4,
    LA_S5,
    LA_S6,
    LA_S7,
    LA_S8,
    LA_R0 = 0,
    LA_R1,
    LA_R2,
    LA_R3,
    LA_R4,
    LA_R5,
    LA_R6,
    LA_R7,
    LA_R8,
    LA_R9,
    LA_R10,
    LA_R11,
    LA_R12,
    LA_R13,
    LA_R14,
    LA_R15,
    LA_R16,
    LA_R17,
    LA_R18,
    LA_R19,
    LA_R20,
    LA_R21,
    LA_R22,
    LA_R23,
    LA_R24,
    LA_R25,
    LA_R26,
    LA_R27,
    LA_R28,
    LA_R29,
    LA_R30,
    LA_R31,
    LA_GPR_COUNT
} la_gpr_t;

typedef enum {
    LA_FA0 = 0,
    LA_FA1,
    LA_FA2,
    LA_FA3,
    LA_FA4,
    LA_FA5,
    LA_FA6,
    LA_FA7,
    LA_FT0,
    LA_FT1,
    LA_FT2,
    LA_FT3,
    LA_FT4,
    LA_FT5,
    LA_FT6,
    LA_FT7,
    LA_FT8,
    LA_FT9,
    LA_FT10,
    LA_FT11,
    LA_FT12,
    LA_FT13,
    LA_FT14,
    LA_FT15,
    LA_FS0,
    LA_FS1,
    LA_FS2,
    LA_FS3,
    LA_FS4,
    LA_FS5,
    LA_FS6,
    LA_FS7,
    LA_F0 = 0,
    LA_F1,
    LA_F2,
    LA_F3,
    LA_F4,
    LA_F5,
    LA_F6,
    LA_F7,
    LA_F8,
    LA_F9,
    LA_F10,
    LA_F11,
    LA_F12,
    LA_F13,
    LA_F14,
    LA_F15,
    LA_F16,
    LA_F17,
    LA_F18,
    LA_F19,
    LA_F20,
    LA_F21,
    LA_F22,
    LA_F23,
    LA_F24,
    LA_F25,
    LA_F26,
    LA_F27,
    LA_F28,
    LA_F29,
    LA_F30,
    LA_F31,
    LA_FPR_COUNT
} la_fpr_t;

typedef enum {
    LA_V0 = 0,
    LA_V1,
    LA_V2,
    LA_V3,
    LA_V4,
    LA_V5,
    LA_V6,
    LA_V7,
    LA_V8,
    LA_V9,
    LA_V10,
    LA_V11,
    LA_V12,
    LA_V13,
    LA_V14,
    LA_V15,
    LA_V16,
    LA_V17,
    LA_V18,
    LA_V19,
    LA_V20,
    LA_V21,
    LA_V22,
    LA_V23,
    LA_V24,
    LA_V25,
    LA_V26,
    LA_V27,
    LA_V28,
    LA_V29,
    LA_V30,
    LA_V31,
    LA_VSR_COUNT
} la_vpr_t;

typedef enum {
    LA_XV0 = 0,
    LA_XV1,
    LA_XV2,
    LA_XV3,
    LA_XV4,
    LA_XV5,
    LA_XV6,
    LA_XV7,
    LA_XV8,
    LA_XV9,
    LA_XV10,
    LA_XV11,
    LA_XV12,
    LA_XV13,
    LA_XV14,
    LA_XV15,
    LA_XV16,
    LA_XV17,
    LA_XV18,
    LA_XV19,
    LA_XV20,
    LA_XV21,
    LA_XV22,
    LA_XV23,
    LA_XV24,
    LA_XV25,
    LA_XV26,
    LA_XV27,
    LA_XV28,
    LA_XV29,
    LA_XV30,
    LA_XV31,
    LA_XVSR_COUNT
} la_xvpr_t;

typedef enum {
    LA_FCC0 = 0,
    LA_FCC1,
    LA_FCC2,
    LA_FCC3,
    LA_FCC4,
    LA_FCC5,
    LA_FCC6,
    LA_FCC7,
    LA_FCC_COUNT
} la_fcc_t;

typedef enum {
    LA_SCR0 = 0,
    LA_SCR1,
    LA_SCR2,
    LA_SCR3,
    LA_SCR_COUNT
} la_scr_t;

typedef enum {
    LA_FCSR0 = 0,
    LA_FCSR1,
    LA_FCSR2,
    LA_FCSR3,
    LA_FCSR_COUNT
} la_fcsr_t;

typedef struct {
    ptrdiff_t location;
    ptrdiff_t* offsets;
    size_t offset_capacity;
    size_t offset_count;
    bool is_bound;
} lagoon_label_t;

typedef struct {
    uint8_t* buffer;
    uint8_t* cursor;
    size_t capacity;
} lagoon_assembler_t;

void la_init_assembler(lagoon_assembler_t* assembler, uint8_t* buffer, size_t capacity);
size_t la_get_remaining_buffer_size(lagoon_assembler_t* assembler);
void la_bind(lagoon_assembler_t* assembler, lagoon_label_t* label);
ptrdiff_t la_label(lagoon_assembler_t* assembler, lagoon_label_t* label);
void la_label_free(lagoon_assembler_t* assembler, lagoon_label_t* label);

void la_load_immediate32(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t value);
void la_load_immediate64(lagoon_assembler_t* assembler, la_gpr_t rd, int64_t value);
void la_ret(lagoon_assembler_t* assembler);

// clang-format off
//// ANCHOR: mnemonic function declarations start

void la_adc_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_adc_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_adc_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_adc_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_add_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_add_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_addi_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_addi_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_addu12i_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk5);
void la_addu12i_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk5);
void la_addu16i_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk16);
void la_alsl_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk, uint32_t ua2pp1);
void la_alsl_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk, uint32_t ua2pp1);
void la_alsl_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk, uint32_t ua2pp1);
void la_amadd_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_db_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_db_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amadd_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amand_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amand_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amand_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amand_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_db_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_db_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amcas_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_db_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_db_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammax_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_db_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_db_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_ammin_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amor_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amor_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amor_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amor_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_db_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_db_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amswap_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amxor_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amxor_db_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amxor_db_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_amxor_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_and(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_andi(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk12);
void la_andn(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_armadc_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armadd_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armand_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armmfflag(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uk8);
void la_armmov_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_armmov_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_armmove(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk4);
void la_armmtflag(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uk8);
void la_armnot_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_armor_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armrotr_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armrotri_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5, uint32_t ud4);
void la_armrrx_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_armsbc_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armsll_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armslli_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5, uint32_t ud4);
void la_armsra_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armsrai_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5, uint32_t ud4);
void la_armsrl_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armsrli_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5, uint32_t ud4);
void la_armsub_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_armxor_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk, uint32_t ud4);
void la_asrtgt_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_asrtle_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_b(lagoon_assembler_t* assembler, int32_t sd10k16ps2);
void la_bceqz(lagoon_assembler_t* assembler, la_fcc_t cj, int32_t sd5k16ps2);
void la_bcnez(lagoon_assembler_t* assembler, la_fcc_t cj, int32_t sd5k16ps2);
void la_beq(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rd, int32_t sk16ps2);
void la_beqz(lagoon_assembler_t* assembler, la_gpr_t rj, int32_t sd5k16ps2);
void la_bge(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rd, int32_t sk16ps2);
void la_bgeu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rd, int32_t sk16ps2);
void la_bitrev_4b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_bitrev_8b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_bitrev_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_bitrev_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_bl(lagoon_assembler_t* assembler, int32_t sd10k16ps2);
void la_blt(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rd, int32_t sk16ps2);
void la_bltu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rd, int32_t sk16ps2);
void la_bne(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rd, int32_t sk16ps2);
void la_bnez(lagoon_assembler_t* assembler, la_gpr_t rj, int32_t sd5k16ps2);
void la_break(lagoon_assembler_t* assembler, uint32_t ud15);
void la_bstrins_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t um6, uint32_t uk6);
void la_bstrins_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t um5, uint32_t uk5);
void la_bstrpick_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t um6, uint32_t uk6);
void la_bstrpick_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t um5, uint32_t uk5);
void la_bytepick_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk, uint32_t ua3);
void la_bytepick_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk, uint32_t ua2);
void la_cacop(lagoon_assembler_t* assembler, uint32_t ud5, la_gpr_t rj, int32_t sk12);
void la_clo_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_clo_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_clz_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_clz_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_cpucfg(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_crc_w_b_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crc_w_d_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crc_w_h_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crc_w_w_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crcc_w_b_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crcc_w_d_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crcc_w_h_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_crcc_w_w_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_csrxchg(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk14);
void la_cto_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_cto_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_ctz_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_ctz_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_dbar(lagoon_assembler_t* assembler, uint32_t ud15);
void la_dbcl(lagoon_assembler_t* assembler, uint32_t ud15);
void la_div_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_div_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_div_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_div_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ertn(lagoon_assembler_t* assembler);
void la_ext_w_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_ext_w_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_fabs_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fabs_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fadd_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fadd_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fclass_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fclass_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fcmp_caf_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_caf_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_ceq_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_ceq_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cle_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cle_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_clt_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_clt_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cne_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cne_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cor_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cor_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cueq_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cueq_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cule_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cule_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cult_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cult_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cun_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cun_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cune_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_cune_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_saf_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_saf_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_seq_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_seq_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sle_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sle_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_slt_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_slt_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sne_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sne_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sor_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sor_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sueq_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sueq_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sule_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sule_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sult_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sult_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sun_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sun_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sune_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcmp_sune_s(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj, la_fpr_t fk);
void la_fcopysign_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fcopysign_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fcvt_d_ld(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fcvt_d_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fcvt_ld_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fcvt_s_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fcvt_ud_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fdiv_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fdiv_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_ffint_d_l(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ffint_d_w(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ffint_s_l(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ffint_s_w(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fld_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, int32_t sk12);
void la_fld_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, int32_t sk12);
void la_fldgt_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fldgt_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fldle_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fldle_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fldx_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fldx_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_flogb_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_flogb_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fmadd_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fmadd_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fmax_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmax_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmaxa_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmaxa_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmin_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmin_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmina_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmina_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmov_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fmov_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fmsub_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fmsub_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fmul_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fmul_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fneg_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fneg_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fnmadd_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fnmadd_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fnmsub_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_fnmsub_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fpr_t fa);
void la_frecip_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frecip_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frecipe_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frecipe_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frint_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frint_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frsqrt_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frsqrt_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frsqrte_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_frsqrte_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fscaleb_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fscaleb_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fsel(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk, la_fcc_t ca);
void la_fsqrt_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fsqrt_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_fst_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, int32_t sk12);
void la_fst_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, int32_t sk12);
void la_fstgt_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fstgt_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fstle_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fstle_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fstx_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fstx_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj, la_gpr_t rk);
void la_fsub_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_fsub_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj, la_fpr_t fk);
void la_ftint_l_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftint_l_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftint_w_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftint_w_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrm_l_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrm_l_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrm_w_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrm_w_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrne_l_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrne_l_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrne_w_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrne_w_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrp_l_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrp_l_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrp_w_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrp_w_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrz_l_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrz_l_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrz_w_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_ftintrz_w_s(lagoon_assembler_t* assembler, la_fpr_t fd, la_fpr_t fj);
void la_gcsrxchg(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk14);
void la_gtlbclr(lagoon_assembler_t* assembler);
void la_gtlbfill(lagoon_assembler_t* assembler);
void la_gtlbflush(lagoon_assembler_t* assembler);
void la_gtlbrd(lagoon_assembler_t* assembler);
void la_gtlbsrch(lagoon_assembler_t* assembler);
void la_gtlbwr(lagoon_assembler_t* assembler);
void la_hvcl(lagoon_assembler_t* assembler, uint32_t ud15);
void la_ibar(lagoon_assembler_t* assembler, uint32_t ud15);
void la_idle(lagoon_assembler_t* assembler, uint32_t ud15);
void la_invtlb(lagoon_assembler_t* assembler, uint32_t ud5, la_gpr_t rj, la_gpr_t rk);
void la_iocsrrd_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrrd_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrrd_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrrd_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrwr_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrwr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrwr_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_iocsrwr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_jirl(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk16ps2);
void la_jiscr0(lagoon_assembler_t* assembler, int32_t sd5k16ps2);
void la_jiscr1(lagoon_assembler_t* assembler, int32_t sd5k16ps2);
void la_ld_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ld_bu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ld_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ld_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ld_hu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ld_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ld_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_lddir(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk8);
void la_ldgt_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldgt_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldgt_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldgt_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldl_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ldl_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ldle_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldle_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldle_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldle_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldpte(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk8);
void la_ldptr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_ldptr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_ldr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ldr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_ldx_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldx_bu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldx_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldx_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldx_hu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldx_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ldx_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ll_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_ll_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_llacq_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_llacq_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_lu12i_w(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t sj20);
void la_lu32i_d(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t sj20);
void la_lu52i_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_maskeqz(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_masknez(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mod_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mod_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mod_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mod_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_movcf2fr(lagoon_assembler_t* assembler, la_fpr_t fd, la_fcc_t cj);
void la_movcf2gr(lagoon_assembler_t* assembler, la_gpr_t rd, la_fcc_t cj);
void la_movfcsr2gr(lagoon_assembler_t* assembler, la_gpr_t rd, la_fcsr_t rj);
void la_movfr2cf(lagoon_assembler_t* assembler, la_fcc_t cd, la_fpr_t fj);
void la_movfr2gr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_fpr_t fj);
void la_movfr2gr_s(lagoon_assembler_t* assembler, la_gpr_t rd, la_fpr_t fj);
void la_movfrh2gr_s(lagoon_assembler_t* assembler, la_gpr_t rd, la_fpr_t fj);
void la_movgr2cf(lagoon_assembler_t* assembler, la_fcc_t cd, la_gpr_t rj);
void la_movgr2fcsr(lagoon_assembler_t* assembler, la_fcsr_t rd, la_gpr_t rj);
void la_movgr2fr_d(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj);
void la_movgr2fr_w(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj);
void la_movgr2frh_w(lagoon_assembler_t* assembler, la_fpr_t fd, la_gpr_t rj);
void la_movgr2scr(lagoon_assembler_t* assembler, la_scr_t td, la_gpr_t rj);
void la_movscr2gr(lagoon_assembler_t* assembler, la_gpr_t rd, la_scr_t tj);
void la_mul_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mul_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mulh_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mulh_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mulh_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mulh_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mulw_d_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_mulw_d_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_nor(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_or(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_ori(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk12);
void la_orn(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_pcaddi(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t sj20);
void la_pcaddu12i(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t sj20);
void la_pcaddu18i(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t sj20);
void la_pcalau12i(lagoon_assembler_t* assembler, la_gpr_t rd, int32_t sj20);
void la_preld(lagoon_assembler_t* assembler, uint32_t ud5, la_gpr_t rj, int32_t sk12);
void la_preldx(lagoon_assembler_t* assembler, uint32_t ud5, la_gpr_t rj, la_gpr_t rk);
void la_rcr_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rcr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rcr_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rcr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rcri_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk3);
void la_rcri_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk6);
void la_rcri_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk4);
void la_rcri_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk5);
void la_rdtime_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_rdtimeh_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_rdtimel_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_revb_2h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_revb_2w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_revb_4h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_revb_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_revh_2w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_revh_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_rotr_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rotr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rotr_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rotr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_rotri_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk3);
void la_rotri_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk6);
void la_rotri_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk4);
void la_rotri_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk5);
void la_sbc_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sbc_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sbc_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sbc_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sc_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_sc_q(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rk, la_gpr_t rj);
void la_sc_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_screl_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_screl_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_setarmj(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uk4);
void la_setx86j(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uk4);
void la_setx86loope(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_setx86loopne(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj);
void la_sll_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sll_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_slli_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk6);
void la_slli_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk5);
void la_slt(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_slti(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_sltu(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sltui(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_sra_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sra_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_srai_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk6);
void la_srai_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk5);
void la_srl_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_srl_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_srli_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk6);
void la_srli_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk5);
void la_st_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_st_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_st_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_st_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_stgt_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stgt_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stgt_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stgt_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stl_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_stl_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_stle_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stle_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stle_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stle_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stptr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_stptr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk14ps2);
void la_str_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_str_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, int32_t sk12);
void la_stx_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stx_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stx_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_stx_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sub_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_sub_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_syscall(lagoon_assembler_t* assembler, uint32_t ud15);
void la_tlbclr(lagoon_assembler_t* assembler);
void la_tlbfill(lagoon_assembler_t* assembler);
void la_tlbflush(lagoon_assembler_t* assembler);
void la_tlbrd(lagoon_assembler_t* assembler);
void la_tlbsrch(lagoon_assembler_t* assembler);
void la_tlbwr(lagoon_assembler_t* assembler);
void la_vabsd_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vabsd_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadd_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadd_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadd_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadd_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadda_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadda_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadda_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vadda_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddi_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vaddi_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vaddi_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vaddi_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vaddwev_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_d_wu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_h_bu_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_q_du_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwev_w_hu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_d_wu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_h_bu_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_q_du_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vaddwod_w_hu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vand_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vandi_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vandn_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavg_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vavgr_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitclr_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitclr_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitclr_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitclr_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitclri_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vbitclri_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vbitclri_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vbitclri_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vbitrev_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitrev_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitrev_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitrev_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitrevi_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vbitrevi_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vbitrevi_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vbitrevi_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vbitsel_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vbitseli_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vbitset_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitset_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitset_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitset_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vbitseti_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vbitseti_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vbitseti_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vbitseti_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vbsll_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vbsrl_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vclo_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclo_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclo_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclo_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclz_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclz_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclz_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vclz_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vdiv_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vdiv_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vext2xv_d_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_d_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_du_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_du_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_du_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_hu_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_w_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_wu_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vext2xv_wu_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_vexth_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_du_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_hu_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_qu_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vexth_wu_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vextl_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vextl_qu_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vextrins_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vextrins_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vextrins_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vextrins_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vfadd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfadd_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfclass_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfclass_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfcmp_caf_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_caf_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_ceq_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_ceq_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cle_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cle_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_clt_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_clt_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cne_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cne_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cor_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cor_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cueq_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cueq_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cule_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cule_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cult_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cult_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cun_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cun_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cune_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_cune_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_saf_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_saf_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_seq_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_seq_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sle_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sle_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_slt_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_slt_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sne_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sne_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sor_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sor_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sueq_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sueq_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sule_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sule_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sult_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sult_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sun_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sun_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sune_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcmp_sune_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcvt_h_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcvt_s_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfcvth_d_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfcvth_s_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfcvtl_d_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfcvtl_s_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfdiv_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfdiv_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vffint_d_l(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vffint_d_lu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vffint_s_l(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vffint_s_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vffint_s_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vffinth_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vffintl_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vflogb_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vflogb_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfmadd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfmadd_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfmax_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmax_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmaxa_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmaxa_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmin_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmin_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmina_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmina_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmsub_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfmsub_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfmul_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfmul_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfnmadd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfnmadd_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfnmsub_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfnmsub_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vfrecip_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrecip_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrecipe_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrecipe_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrint_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrint_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrm_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrm_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrne_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrne_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrp_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrp_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrz_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrintrz_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrsqrt_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrsqrt_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrsqrte_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrsqrte_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfrstp_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfrstp_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfrstpi_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vfrstpi_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vfsqrt_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfsqrt_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vfsub_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vfsub_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vftint_l_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftint_lu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftint_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vftint_w_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftint_wu_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftinth_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintl_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrm_l_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrm_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vftintrm_w_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrmh_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrml_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrne_l_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrne_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vftintrne_w_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrneh_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrnel_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrp_l_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrp_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vftintrp_w_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrph_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrpl_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrz_l_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrz_lu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrz_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vftintrz_w_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrz_wu_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrzh_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vftintrzl_l_s(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vhaddw_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_du_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_hu_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_qu_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhaddw_wu_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_du_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_hu_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_qu_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vhsubw_wu_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvh_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvh_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvh_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvh_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvl_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvl_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvl_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vilvl_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vinsgr2vr_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, uint32_t uk4);
void la_vinsgr2vr_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, uint32_t uk1);
void la_vinsgr2vr_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, uint32_t uk3);
void la_vinsgr2vr_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, uint32_t uk2);
void la_vld(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk12);
void la_vldi(lagoon_assembler_t* assembler, la_vpr_t vd, int32_t sj13);
void la_vldrepl_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk12);
void la_vldrepl_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk9ps3);
void la_vldrepl_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk11ps1);
void la_vldrepl_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk10ps2);
void la_vldx(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, la_gpr_t rk);
void la_vmadd_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmadd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmadd_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmadd_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_d_wu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_h_bu_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_q_du_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwev_w_hu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_d_wu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_h_bu_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_q_du_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaddwod_w_hu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmax_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmaxi_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmaxi_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmaxi_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmaxi_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmaxi_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmaxi_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmaxi_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmaxi_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmin_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmin_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmini_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmini_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmini_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmini_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmini_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmini_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmini_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vmini_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vmod_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmod_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmskgez_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vmskltz_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vmskltz_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vmskltz_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vmskltz_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vmsknz_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vmsub_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmsub_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmsub_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmsub_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmuh_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmul_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmul_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmul_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmul_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_d_wu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_h_bu_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_q_du_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwev_w_hu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_d_wu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_h_bu_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_q_du_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vmulwod_w_hu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vneg_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vneg_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vneg_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vneg_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vnor_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vnori_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vor_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vori_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vorn_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackev_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackev_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackev_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackev_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackod_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackod_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackod_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpackod_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpcnt_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vpcnt_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vpcnt_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vpcnt_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj);
void la_vpermi_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vpickev_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickev_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickev_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickev_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickod_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickod_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickod_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickod_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vpickve2gr_b(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk4);
void la_vpickve2gr_bu(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk4);
void la_vpickve2gr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk1);
void la_vpickve2gr_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk1);
void la_vpickve2gr_h(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk3);
void la_vpickve2gr_hu(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk3);
void la_vpickve2gr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk2);
void la_vpickve2gr_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_vpr_t vj, uint32_t uk2);
void la_vreplgr2vr_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj);
void la_vreplgr2vr_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj);
void la_vreplgr2vr_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj);
void la_vreplgr2vr_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj);
void la_vreplve_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_gpr_t rk);
void la_vreplve_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_gpr_t rk);
void la_vreplve_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_gpr_t rk);
void la_vreplve_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_gpr_t rk);
void la_vreplvei_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vreplvei_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk1);
void la_vreplvei_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vreplvei_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk2);
void la_vrotr_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vrotr_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vrotr_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vrotr_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vrotri_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vrotri_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vrotri_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vrotri_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsadd_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsadd_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsat_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsat_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsat_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsat_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsat_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsat_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsat_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsat_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vseq_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vseq_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vseq_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vseq_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vseqi_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vseqi_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vseqi_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vseqi_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vsetallnez_b(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetallnez_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetallnez_h(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetallnez_w(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetanyeqz_b(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetanyeqz_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetanyeqz_h(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetanyeqz_w(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vseteqz_v(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vsetnez_v(lagoon_assembler_t* assembler, la_fcc_t cd, la_vpr_t vj);
void la_vshuf4i_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vshuf4i_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vshuf4i_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vshuf4i_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_vshuf_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk, la_vpr_t va);
void la_vshuf_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vshuf_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vshuf_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsigncov_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsigncov_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsigncov_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsigncov_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsle_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslei_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslei_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vslei_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslei_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vslei_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslei_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vslei_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslei_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsll_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsll_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsll_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsll_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslli_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vslli_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vslli_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vslli_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsllwil_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsllwil_du_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsllwil_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsllwil_hu_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsllwil_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsllwil_wu_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vslt_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslt_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vslti_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslti_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vslti_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslti_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vslti_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslti_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vslti_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, int32_t sk5);
void la_vslti_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsra_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsra_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsra_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsra_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrai_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsrai_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrai_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrai_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsran_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsran_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsran_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrani_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrani_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vsrani_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrani_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrar_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrar_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrar_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrar_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrari_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsrari_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrari_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrari_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrarn_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrarn_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrarn_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrarni_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrarni_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vsrarni_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrarni_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrl_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrl_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrl_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrl_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrli_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsrli_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrli_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrli_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrln_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrln_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrln_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlni_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrlni_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vsrlni_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrlni_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrlr_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlr_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlr_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlr_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlri_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk3);
void la_vsrlri_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vsrlri_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrlri_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrlrn_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlrn_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlrn_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsrlrni_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vsrlrni_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vsrlrni_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsrlrni_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssran_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssran_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssran_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssran_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssran_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssran_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrani_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrani_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrani_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrani_du_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrani_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrani_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrani_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrani_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrarn_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrarn_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrarn_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrarn_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrarn_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrarn_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrarni_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrarni_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrarni_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrarni_du_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrarni_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrarni_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrarni_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrarni_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrln_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrln_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrln_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrln_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrln_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrln_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlni_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrlni_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrlni_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrlni_du_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrlni_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrlni_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrlni_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrlni_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrlrn_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlrn_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlrn_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlrn_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlrn_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlrn_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssrlrni_b_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrlrni_bu_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk4);
void la_vssrlrni_d_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrlrni_du_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk7);
void la_vssrlrni_h_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrlrni_hu_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vssrlrni_w_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssrlrni_wu_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk6);
void la_vssub_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vssub_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vst(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk12);
void la_vstelm_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk8, uint32_t un4);
void la_vstelm_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk8ps3, uint32_t un1);
void la_vstelm_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk8ps1, uint32_t un3);
void la_vstelm_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, int32_t sk8ps2, uint32_t un2);
void la_vstx(lagoon_assembler_t* assembler, la_vpr_t vd, la_gpr_t rj, la_gpr_t rk);
void la_vsub_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsub_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsub_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsub_q(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsub_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubi_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsubi_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsubi_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsubi_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk5);
void la_vsubwev_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwev_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_d_w(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_d_wu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_h_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_h_bu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_q_d(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_q_du(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_w_h(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vsubwod_w_hu(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vxor_v(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, la_vpr_t vk);
void la_vxori_b(lagoon_assembler_t* assembler, la_vpr_t vd, la_vpr_t vj, uint32_t uk8);
void la_x86adc_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86adc_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86adc_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86adc_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86add_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86add_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86add_du(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86add_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86add_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86add_wu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86and_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86and_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86and_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86and_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86clrtm(lagoon_assembler_t* assembler);
void la_x86dec_b(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86dec_d(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86dec_h(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86dec_w(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86dectop(lagoon_assembler_t* assembler);
void la_x86inc_b(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86inc_d(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86inc_h(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86inc_w(lagoon_assembler_t* assembler, la_gpr_t rj);
void la_x86inctop(lagoon_assembler_t* assembler);
void la_x86mfflag(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uk8);
void la_x86mftop(lagoon_assembler_t* assembler, la_gpr_t rd);
void la_x86mtflag(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uk8);
void la_x86mttop(lagoon_assembler_t* assembler, uint32_t uj3);
void la_x86mul_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_bu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_du(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_hu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86mul_wu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86or_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86or_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86or_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86or_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcl_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcl_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcl_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcl_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcli_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86rcli_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86rcli_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86rcli_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86rcr_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcr_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcr_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcr_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rcri_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86rcri_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86rcri_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86rcri_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86rotl_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotl_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotl_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotl_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotli_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86rotli_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86rotli_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86rotli_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86rotr_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotr_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotr_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotr_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86rotri_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86rotri_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86rotri_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86rotri_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86sbc_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sbc_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sbc_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sbc_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86settag(lagoon_assembler_t* assembler, la_gpr_t rd, uint32_t uj5, uint32_t uk8);
void la_x86settm(lagoon_assembler_t* assembler);
void la_x86sll_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sll_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sll_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sll_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86slli_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86slli_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86slli_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86slli_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86sra_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sra_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sra_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sra_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86srai_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86srai_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86srai_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86srai_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86srl_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86srl_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86srl_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86srl_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86srli_b(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk3);
void la_x86srli_d(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk6);
void la_x86srli_h(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk4);
void la_x86srli_w(lagoon_assembler_t* assembler, la_gpr_t rj, uint32_t uk5);
void la_x86sub_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sub_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sub_du(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sub_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sub_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86sub_wu(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86xor_b(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86xor_d(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86xor_h(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_x86xor_w(lagoon_assembler_t* assembler, la_gpr_t rj, la_gpr_t rk);
void la_xor(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, la_gpr_t rk);
void la_xori(lagoon_assembler_t* assembler, la_gpr_t rd, la_gpr_t rj, uint32_t uk12);
void la_xvabsd_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvabsd_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadd_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadd_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadd_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadd_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadda_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadda_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadda_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvadda_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddi_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvaddi_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvaddi_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvaddi_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvaddwev_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_d_wu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_h_bu_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_q_du_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwev_w_hu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_d_wu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_h_bu_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_q_du_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvaddwod_w_hu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvand_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvandi_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvandn_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavg_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvavgr_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitclr_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitclr_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitclr_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitclr_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitclri_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvbitclri_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvbitclri_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvbitclri_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvbitrev_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitrev_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitrev_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitrev_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitrevi_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvbitrevi_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvbitrevi_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvbitrevi_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvbitsel_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvbitseli_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvbitset_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitset_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitset_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitset_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvbitseti_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvbitseti_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvbitseti_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvbitseti_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvbsll_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvbsrl_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvclo_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclo_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclo_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclo_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclz_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclz_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclz_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvclz_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvdiv_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvdiv_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvexth_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_du_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_hu_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_qu_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvexth_wu_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvextl_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvextl_qu_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvextrins_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvextrins_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvextrins_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvextrins_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvfadd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfadd_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfclass_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfclass_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfcmp_caf_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_caf_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_ceq_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_ceq_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cle_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cle_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_clt_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_clt_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cne_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cne_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cor_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cor_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cueq_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cueq_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cule_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cule_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cult_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cult_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cun_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cun_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cune_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_cune_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_saf_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_saf_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_seq_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_seq_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sle_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sle_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_slt_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_slt_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sne_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sne_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sor_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sor_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sueq_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sueq_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sule_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sule_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sult_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sult_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sun_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sun_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sune_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcmp_sune_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcvt_h_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcvt_s_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfcvth_d_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfcvth_s_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfcvtl_d_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfcvtl_s_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfdiv_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfdiv_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvffint_d_l(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvffint_d_lu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvffint_s_l(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvffint_s_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvffint_s_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvffinth_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvffintl_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvflogb_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvflogb_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfmadd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfmadd_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfmax_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmax_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmaxa_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmaxa_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmin_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmin_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmina_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmina_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmsub_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfmsub_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfmul_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfmul_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfnmadd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfnmadd_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfnmsub_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfnmsub_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvfrecip_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrecip_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrecipe_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrecipe_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrint_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrint_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrm_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrm_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrne_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrne_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrp_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrp_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrz_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrintrz_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrsqrt_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrsqrt_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrsqrte_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrsqrte_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfrstp_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfrstp_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfrstpi_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvfrstpi_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvfsqrt_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfsqrt_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvfsub_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvfsub_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvftint_l_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftint_lu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftint_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvftint_w_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftint_wu_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftinth_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintl_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrm_l_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrm_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvftintrm_w_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrmh_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrml_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrne_l_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrne_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvftintrne_w_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrneh_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrnel_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrp_l_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrp_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvftintrp_w_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrph_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrpl_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrz_l_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrz_lu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrz_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvftintrz_w_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrz_wu_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrzh_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvftintrzl_l_s(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvhaddw_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_du_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_hu_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_qu_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhaddw_wu_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_du_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_hu_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_qu_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvhsubw_wu_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvh_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvh_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvh_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvh_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvl_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvl_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvl_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvilvl_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvinsgr2vr_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, uint32_t uk2);
void la_xvinsgr2vr_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, uint32_t uk3);
void la_xvinsve0_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk2);
void la_xvinsve0_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvld(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk12);
void la_xvldi(lagoon_assembler_t* assembler, la_xvpr_t xd, int32_t sj13);
void la_xvldrepl_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk12);
void la_xvldrepl_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk9ps3);
void la_xvldrepl_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk11ps1);
void la_xvldrepl_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk10ps2);
void la_xvldx(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, la_gpr_t rk);
void la_xvmadd_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmadd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmadd_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmadd_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_d_wu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_h_bu_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_q_du_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwev_w_hu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_d_wu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_h_bu_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_q_du_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaddwod_w_hu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmax_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmaxi_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmaxi_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmaxi_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmaxi_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmaxi_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmaxi_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmaxi_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmaxi_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmin_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmin_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmini_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmini_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmini_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmini_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmini_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmini_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmini_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvmini_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvmod_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmod_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmskgez_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvmskltz_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvmskltz_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvmskltz_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvmskltz_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvmsknz_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvmsub_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmsub_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmsub_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmsub_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmuh_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmul_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmul_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmul_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmul_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_d_wu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_h_bu_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_q_du_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwev_w_hu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_d_wu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_h_bu_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_q_du_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvmulwod_w_hu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvneg_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvneg_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvneg_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvneg_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvnor_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvnori_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvor_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvori_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvorn_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackev_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackev_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackev_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackev_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackod_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackod_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackod_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpackod_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpcnt_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvpcnt_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvpcnt_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvpcnt_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvperm_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpermi_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvpermi_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvpermi_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvpickev_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickev_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickev_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickev_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickod_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickod_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickod_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickod_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvpickve2gr_d(lagoon_assembler_t* assembler, la_gpr_t rd, la_xvpr_t xj, uint32_t uk2);
void la_xvpickve2gr_du(lagoon_assembler_t* assembler, la_gpr_t rd, la_xvpr_t xj, uint32_t uk2);
void la_xvpickve2gr_w(lagoon_assembler_t* assembler, la_gpr_t rd, la_xvpr_t xj, uint32_t uk3);
void la_xvpickve2gr_wu(lagoon_assembler_t* assembler, la_gpr_t rd, la_xvpr_t xj, uint32_t uk3);
void la_xvpickve_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk2);
void la_xvpickve_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvrepl128vei_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvrepl128vei_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk1);
void la_xvrepl128vei_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvrepl128vei_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk2);
void la_xvreplgr2vr_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj);
void la_xvreplgr2vr_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj);
void la_xvreplgr2vr_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj);
void la_xvreplgr2vr_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj);
void la_xvreplve0_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvreplve0_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvreplve0_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvreplve0_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvreplve0_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj);
void la_xvreplve_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_gpr_t rk);
void la_xvreplve_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_gpr_t rk);
void la_xvreplve_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_gpr_t rk);
void la_xvreplve_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_gpr_t rk);
void la_xvrotr_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvrotr_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvrotr_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvrotr_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvrotri_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvrotri_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvrotri_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvrotri_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsadd_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsadd_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsat_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsat_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsat_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsat_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsat_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsat_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsat_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsat_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvseq_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvseq_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvseq_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvseq_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvseqi_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvseqi_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvseqi_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvseqi_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvsetallnez_b(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetallnez_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetallnez_h(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetallnez_w(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetanyeqz_b(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetanyeqz_d(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetanyeqz_h(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetanyeqz_w(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvseteqz_v(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvsetnez_v(lagoon_assembler_t* assembler, la_fcc_t cd, la_xvpr_t xj);
void la_xvshuf4i_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvshuf4i_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvshuf4i_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvshuf4i_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xvshuf_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk, la_xvpr_t xa);
void la_xvshuf_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvshuf_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvshuf_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsigncov_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsigncov_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsigncov_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsigncov_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsle_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslei_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslei_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvslei_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslei_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvslei_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslei_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvslei_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslei_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsll_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsll_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsll_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsll_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslli_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvslli_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvslli_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvslli_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsllwil_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsllwil_du_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsllwil_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsllwil_hu_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsllwil_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsllwil_wu_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvslt_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslt_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvslti_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslti_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvslti_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslti_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvslti_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslti_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvslti_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, int32_t sk5);
void la_xvslti_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsra_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsra_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsra_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsra_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrai_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsrai_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrai_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrai_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsran_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsran_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsran_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrani_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrani_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvsrani_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrani_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrar_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrar_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrar_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrar_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrari_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsrari_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrari_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrari_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrarn_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrarn_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrarn_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrarni_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrarni_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvsrarni_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrarni_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrl_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrl_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrl_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrl_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrli_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsrli_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrli_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrli_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrln_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrln_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrln_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlni_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrlni_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvsrlni_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrlni_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrlr_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlr_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlr_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlr_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlri_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk3);
void la_xvsrlri_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvsrlri_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrlri_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrlrn_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlrn_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlrn_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsrlrni_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvsrlrni_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvsrlrni_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsrlrni_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssran_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssran_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssran_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssran_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssran_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssran_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrani_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrani_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrani_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrani_du_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrani_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrani_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrani_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrani_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrarn_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrarn_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrarn_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrarn_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrarn_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrarn_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrarni_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrarni_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrarni_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrarni_du_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrarni_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrarni_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrarni_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrarni_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrln_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrln_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrln_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrln_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrln_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrln_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlni_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrlni_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrlni_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrlni_du_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrlni_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrlni_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrlni_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrlni_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrlrn_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlrn_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlrn_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlrn_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlrn_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlrn_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssrlrni_b_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrlrni_bu_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk4);
void la_xvssrlrni_d_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrlrni_du_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk7);
void la_xvssrlrni_h_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrlrni_hu_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvssrlrni_w_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssrlrni_wu_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk6);
void la_xvssub_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvssub_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvst(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk12);
void la_xvstelm_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk8, uint32_t un5);
void la_xvstelm_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk8ps3, uint32_t un2);
void la_xvstelm_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk8ps1, uint32_t un4);
void la_xvstelm_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, int32_t sk8ps2, uint32_t un3);
void la_xvstx(lagoon_assembler_t* assembler, la_xvpr_t xd, la_gpr_t rj, la_gpr_t rk);
void la_xvsub_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsub_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsub_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsub_q(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsub_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubi_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsubi_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsubi_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsubi_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk5);
void la_xvsubwev_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwev_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_d_w(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_d_wu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_h_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_h_bu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_q_d(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_q_du(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_w_h(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvsubwod_w_hu(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvxor_v(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, la_xvpr_t xk);
void la_xvxori_b(lagoon_assembler_t* assembler, la_xvpr_t xd, la_xvpr_t xj, uint32_t uk8);
void la_xxx_unknown_1(lagoon_assembler_t* assembler);

//// ANCHOR: mnemonic function declarations end

// clang-format on

typedef enum {
    LA_OP_GPR,
    LA_OP_FPR,
    LA_OP_VPR,
    LA_OP_XVPR,
    LA_OP_FCC,
    LA_OP_SCR,
    LA_OP_FCSR,
    LA_OP_SIMM,
    LA_OP_UIMM,
} la_operand_kind_t;

typedef struct {
    la_operand_kind_t kind;
    union {
        la_gpr_t gpr;
        la_fpr_t fpr;
        la_vpr_t vpr;
        la_xvpr_t xvpr;
        la_fcc_t fcc;
        la_scr_t scr;
        la_fcsr_t fcsr;
        int32_t simm;
        uint32_t uimm;
    };
} la_operand_t;

#define LA_MAX_OPERANDS 4

typedef struct {
    size_t offset;
    uint32_t encoding;
    const char* mnemonic;
    int operand_count;
    la_operand_t operands[LA_MAX_OPERANDS];
} lagoon_insn_t;

void la_disasm_one(uint32_t word, lagoon_insn_t* insn);
void la_insn_to_str(const lagoon_insn_t* insn, char* buf, size_t buf_size);

bool la_is_store_instruction(uint32_t insn);

#endif // LAGOON_H