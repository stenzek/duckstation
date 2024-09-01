// SPDX-FileCopyrightText: 2016 iCatButler, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0
//
// This file has been completely rewritten over the years compared to the original PCSXR-PGXP release.
// No original code remains. The original copyright notice is included above for historical purposes.
//

#include "cpu_pgxp.h"
#include "bus.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "settings.h"

#include "util/gpu_device.h"

#include "common/assert.h"
#include "common/log.h"

#include <climits>
#include <cmath>

Log_SetChannel(CPU::PGXP);

// #define LOG_VALUES 1
// #define LOG_LOOKUPS 1

// TODO: Don't update flags on Validate(), instead return it.

namespace CPU::PGXP {

enum : u32
{
  VERTEX_CACHE_WIDTH = 2048,
  VERTEX_CACHE_HEIGHT = 2048,
  VERTEX_CACHE_SIZE = VERTEX_CACHE_WIDTH * VERTEX_CACHE_HEIGHT,
  PGXP_MEM_SIZE = (static_cast<u32>(Bus::RAM_8MB_SIZE) + static_cast<u32>(CPU::SCRATCHPAD_SIZE)) / 4,
  PGXP_MEM_SCRATCH_OFFSET = Bus::RAM_8MB_SIZE / 4,
};

enum : u32
{
  VALID_X = (1u << 0),
  VALID_Y = (1u << 1),
  VALID_Z = (1u << 2),
  VALID_LOWZ = (1u << 16),      // Valid Z from the low part of a 32-bit value.
  VALID_HIGHZ = (1u << 17),     // Valid Z from the high part of a 32-bit value.
  VALID_TAINTED_Z = (1u << 31), // X/Y has been changed, Z may not be accurate.

  VALID_XY = (VALID_X | VALID_Y),
  VALID_XYZ = (VALID_X | VALID_Y | VALID_Z),
  VALID_ALL = (VALID_X | VALID_Y | VALID_Z),
};

#define LOWORD_U16(val) (static_cast<u16>(val))
#define HIWORD_U16(val) (static_cast<u16>(static_cast<u32>(val) >> 16))
#define LOWORD_S16(val) (static_cast<s16>(static_cast<u16>(val)))
#define HIWORD_S16(val) (static_cast<s16>(static_cast<u16>(static_cast<u32>(val) >> 16)))
#define SET_LOWORD(val, loword) ((static_cast<u32>(val) & 0xFFFF0000u) | static_cast<u32>(static_cast<u16>(loword)))
#define SET_HIWORD(val, hiword) ((static_cast<u32>(val) & 0x0000FFFFu) | (static_cast<u32>(hiword) << 16))

static double f16Sign(double val);
static double f16Unsign(double val);
static double f16Overflow(double val);

static void CacheVertex(u32 value, const PGXPValue& vertex);
static PGXPValue* GetCachedVertex(u32 value);

static float TruncateVertexPosition(float p);
static bool IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y);

static PGXPValue& GetRdValue(Instruction instr);
static PGXPValue& GetRtValue(Instruction instr);
static PGXPValue& ValidateAndGetRtValue(Instruction instr, u32 rtVal);
static PGXPValue& ValidateAndGetRsValue(Instruction instr, u32 rsVal);
static void SetRtValue(Instruction instr, const PGXPValue& val);
static void SetRtValue(Instruction instr, const PGXPValue& val, u32 rtVal);
static PGXPValue& GetSXY0();
static PGXPValue& GetSXY1();
static PGXPValue& GetSXY2();
static PGXPValue& PushSXY();

static PGXPValue* GetPtr(u32 addr);
static const PGXPValue& ValidateAndLoadMem(u32 addr, u32 value);
static void ValidateAndLoadMem16(PGXPValue& dest, u32 addr, u32 value, bool sign);

static void CPU_MTC2(u32 reg, const PGXPValue& value, u32 val);
static void CPU_BITWISE(Instruction instr, u32 rdVal, u32 rsVal, u32 rtVal);
static void CPU_SLL(Instruction instr, u32 rtVal, u32 sh);
static void CPU_SRx(Instruction instr, u32 rtVal, u32 sh, bool sign, bool is_variable);

static void WriteMem(u32 addr, const PGXPValue& value);
static void WriteMem16(u32 addr, const PGXPValue& value);

static void CopyZIfMissing(PGXPValue& dst, const PGXPValue& src);
static void SelectZ(float& dst_z, u32& dst_flags, const PGXPValue& src1, const PGXPValue& src2);

#ifdef LOG_VALUES
static void LogInstruction(u32 pc, Instruction instr);
static void LogValue(const char* name, u32 rval, const PGXPValue* val);
static void LogValueStr(SmallStringBase& str, const char* name, u32 rval, const PGXPValue* val);

// clang-format off
#define LOG_VALUES_NV() do { LogInstruction(CPU::g_state.current_instruction_pc, instr); } while (0)
#define LOG_VALUES_1(name, rval, val) do { LogInstruction(CPU::g_state.current_instruction_pc, instr); LogValue(name, rval, val); } while (0)
#define LOG_VALUES_C1(rnum, rval) do { LogInstruction(CPU::g_state.current_instruction_pc,instr); LogValue(CPU::GetRegName(static_cast<CPU::Reg>(rnum)), rval, &g_state.pgxp_gpr[static_cast<u32>(rnum)]); } while(0)
#define LOG_VALUES_C2(r1num, r1val, r2num, r2val) do { LogInstruction(CPU::g_state.current_instruction_pc,instr); LogValue(CPU::GetRegName(static_cast<CPU::Reg>(r1num)), r1val, &g_state.pgxp_gpr[static_cast<u32>(r1num)]); LogValue(CPU::GetRegName(static_cast<CPU::Reg>(r2num)), r2val, &g_state.pgxp_gpr[static_cast<u32>(r2num)]); } while(0)
#define LOG_VALUES_LOAD(addr, val) do { LogInstruction(CPU::g_state.current_instruction_pc,instr); LogValue(TinyString::from_format("MEM[{:08X}]", addr).c_str(), val, GetPtr(addr)); } while(0)
#define LOG_VALUES_STORE(rnum, rval, addr) do { LOG_VALUES_C1(rnum, rval); std::fprintf(s_log, " addr=%08X", addr); } while(0)
#else
#define LOG_VALUES_NV() (void)0
#define LOG_VALUES_1(name, rval, val) (void)0
#define LOG_VALUES_C1(rnum, rval) (void)0
#define LOG_VALUES_C2(r1num, r1val, r2num, r2val) (void)0
#define LOG_VALUES_LOAD(addr, val) (void)0
#define LOG_VALUES_STORE(rnum, rval, addr) (void)0
#endif
// clang-format on

static constexpr const PGXPValue INVALID_VALUE = {};

static PGXPValue* s_mem = nullptr;
static PGXPValue* s_vertex_cache = nullptr;

#ifdef LOG_VALUES
static std::FILE* s_log;
#endif
} // namespace CPU::PGXP

void CPU::PGXP::Initialize()
{
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));

  if (!s_mem)
  {
    s_mem = static_cast<PGXPValue*>(std::calloc(PGXP_MEM_SIZE, sizeof(PGXPValue)));
    if (!s_mem)
      Panic("Failed to allocate PGXP memory");
  }

  if (g_settings.gpu_pgxp_vertex_cache && !s_vertex_cache)
  {
    s_vertex_cache = static_cast<PGXPValue*>(std::calloc(VERTEX_CACHE_SIZE, sizeof(PGXPValue)));
    if (!s_vertex_cache)
    {
      ERROR_LOG("Failed to allocate memory for vertex cache, disabling.");
      g_settings.gpu_pgxp_vertex_cache = false;
    }
  }

  if (s_vertex_cache)
    std::memset(s_vertex_cache, 0, sizeof(PGXPValue) * VERTEX_CACHE_SIZE);
}

void CPU::PGXP::Reset()
{
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));

  if (s_mem)
    std::memset(s_mem, 0, sizeof(PGXPValue) * PGXP_MEM_SIZE);

  if (g_settings.gpu_pgxp_vertex_cache && s_vertex_cache)
    std::memset(s_vertex_cache, 0, sizeof(PGXPValue) * VERTEX_CACHE_SIZE);
}

void CPU::PGXP::Shutdown()
{
  if (s_vertex_cache)
  {
    std::free(s_vertex_cache);
    s_vertex_cache = nullptr;
  }
  if (s_mem)
  {
    std::free(s_mem);
    s_mem = nullptr;
  }

  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
}

ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Sign(double val)
{
  const s32 s = static_cast<s32>(static_cast<s64>(val * (USHRT_MAX + 1)));
  return static_cast<double>(s) / static_cast<double>(USHRT_MAX + 1);
}

ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Unsign(double val)
{
  return (val >= 0) ? val : (val + (USHRT_MAX + 1));
}

ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Overflow(double val)
{
  return static_cast<double>(static_cast<s64>(val) >> 16);
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::GetRdValue(Instruction instr)
{
  return g_state.pgxp_gpr[static_cast<u8>(instr.r.rd.GetValue())];
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::GetRtValue(Instruction instr)
{
  return g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())];
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::ValidateAndGetRtValue(Instruction instr, u32 rtVal)
{
  PGXPValue& ret = g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())];
  ret.Validate(rtVal);
  return ret;
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::ValidateAndGetRsValue(Instruction instr, u32 rsVal)
{
  PGXPValue& ret = g_state.pgxp_gpr[static_cast<u8>(instr.r.rs.GetValue())];
  ret.Validate(rsVal);
  return ret;
}

ALWAYS_INLINE void CPU::PGXP::SetRtValue(Instruction instr, const PGXPValue& val)
{
  g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())] = val;
}

ALWAYS_INLINE void CPU::PGXP::SetRtValue(Instruction instr, const PGXPValue& val, u32 rtVal)
{
  PGXPValue& prtVal = g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())];
  prtVal = val;
  prtVal.value = rtVal;
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::GetSXY0()
{
  return g_state.pgxp_gte[12];
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::GetSXY1()
{
  return g_state.pgxp_gte[13];
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::GetSXY2()
{
  return g_state.pgxp_gte[14];
}

ALWAYS_INLINE CPU::PGXPValue& CPU::PGXP::PushSXY()
{
  g_state.pgxp_gte[12] = g_state.pgxp_gte[13];
  g_state.pgxp_gte[13] = g_state.pgxp_gte[14];
  return g_state.pgxp_gte[14];
}

ALWAYS_INLINE_RELEASE CPU::PGXPValue* CPU::PGXP::GetPtr(u32 addr)
{
#if 0
  if ((addr & CPU::PHYSICAL_MEMORY_ADDRESS_MASK) >= 0x0017A2B4 &&
      (addr & CPU::PHYSICAL_MEMORY_ADDRESS_MASK) <= 0x0017A2B4)
    __debugbreak();
#endif

  if ((addr & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR)
    return &s_mem[PGXP_MEM_SCRATCH_OFFSET + ((addr & SCRATCHPAD_OFFSET_MASK) >> 2)];

  const u32 paddr = (addr & PHYSICAL_MEMORY_ADDRESS_MASK);
  if (paddr < Bus::RAM_MIRROR_END)
    return &s_mem[(paddr & Bus::g_ram_mask) >> 2];
  else
    return nullptr;
}

ALWAYS_INLINE_RELEASE const CPU::PGXPValue& CPU::PGXP::ValidateAndLoadMem(u32 addr, u32 value)
{
  PGXPValue* pMem = GetPtr(addr);
  if (!pMem) [[unlikely]]
    return INVALID_VALUE;

  pMem->Validate(value);
  return *pMem;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::ValidateAndLoadMem16(PGXPValue& dest, u32 addr, u32 value, bool sign)
{
  PGXPValue* pMem = GetPtr(addr);
  if (!pMem) [[unlikely]]
  {
    dest = INVALID_VALUE;
    return;
  }

  // determine if high or low word
  const bool hiword = ((addr & 2) != 0);

  // only validate the component we're interested in
  pMem->flags = hiword ?
                  ((Truncate16(pMem->value >> 16) == Truncate16(value)) ? pMem->flags : (pMem->flags & ~VALID_Y)) :
                  ((Truncate16(pMem->value) == Truncate16(value)) ? pMem->flags : (pMem->flags & ~VALID_X));

  // copy whole value
  dest = *pMem;

  // if high word then shift
  if (hiword)
  {
    dest.x = dest.y;
    dest.flags = (dest.flags & ~VALID_X) | ((dest.flags & VALID_Y) >> 1);
  }

  // only set y as valid if x is also valid.. don't want to make fake values
  if (dest.flags & VALID_X)
  {
    dest.y = (dest.x < 0) ? -1.0f * sign : 0.0f;
    dest.flags |= VALID_Y;
  }
  else
  {
    dest.y = 0.0f;
    dest.flags &= ~VALID_Y;
  }

  dest.value = value;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::WriteMem(u32 addr, const PGXPValue& value)
{
  PGXPValue* pMem = GetPtr(addr);
  if (!pMem) [[unlikely]]
    return;

  *pMem = value;
  pMem->flags |= VALID_LOWZ | VALID_HIGHZ;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::WriteMem16(u32 addr, const PGXPValue& value)
{
  PGXPValue* dest = GetPtr(addr);
  if (!dest) [[unlikely]]
    return;

  // determine if high or low word
  const bool hiword = ((addr & 2) != 0);
  if (hiword)
  {
    dest->y = value.x;
    dest->flags = (dest->flags & ~VALID_Y) | ((value.flags & VALID_X) << 1);
    dest->value = (dest->value & UINT32_C(0x0000FFFF)) | (value.value << 16);
  }
  else
  {
    dest->x = value.x;
    dest->flags = (dest->flags & ~VALID_X) | (value.flags & VALID_X);
    dest->value = (dest->value & UINT32_C(0xFFFF0000)) | (value.value & UINT32_C(0x0000FFFF));
  }

  // overwrite z/w if valid
  // TODO: Check modified
  if (value.flags & VALID_Z)
  {
    dest->z = value.z;
    dest->flags |= VALID_Z | (hiword ? VALID_HIGHZ : VALID_LOWZ);
  }
  else
  {
    dest->flags &= hiword ? ~VALID_HIGHZ : ~VALID_LOWZ;
    if (dest->flags & VALID_Z && !(dest->flags & (VALID_HIGHZ | VALID_LOWZ)))
      dest->flags &= ~VALID_Z;
  }
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CopyZIfMissing(PGXPValue& dst, const PGXPValue& src)
{
  dst.z = (dst.flags & VALID_Z) ? dst.z : src.z;
  dst.flags |= (src.flags & VALID_Z);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::SelectZ(float& dst_z, u32& dst_flags, const PGXPValue& src1,
                                              const PGXPValue& src2)
{
  // Prefer src2 if src1 is missing Z, or is potentially an imprecise value, when src2 is precise.
  dst_z = (!(src1.flags & VALID_Z) ||
           (src1.flags & VALID_TAINTED_Z && (src2.flags & (VALID_Z | VALID_TAINTED_Z)) == VALID_Z)) ?
            src2.z :
            src1.z;
  dst_flags |= ((src1.flags | src2.flags) & VALID_Z);
}

#ifdef LOG_VALUES
void CPU::PGXP::LogInstruction(u32 pc, Instruction instr)
{
  if (!s_log) [[unlikely]]
  {
    s_log = std::fopen("pgxp.log", "wb");
  }
  else
  {
    std::fflush(s_log);
    std::fputc('\n', s_log);
  }

  SmallString str;
  DisassembleInstruction(&str, pc, instr.bits);
  std::fprintf(s_log, "%08X %08X %-20s", pc, instr.bits, str.c_str());
}

void CPU::PGXP::LogValue(const char* name, u32 rval, const PGXPValue* val)
{
  if (!s_log) [[unlikely]]
    return;

  SmallString str;
  LogValueStr(str, name, rval, val);
  std::fprintf(s_log, " %s", str.c_str());
}

void CPU::PGXP::LogValueStr(SmallStringBase& str, const char* name, u32 rval, const PGXPValue* val)
{
  str.append_format("{}=[{:08X}", name, rval);
  if (!val)
  {
    str.append(", NULL]");
  }
  else
  {
    if (val->value != rval)
      str.append_format(", PGXP{:08X}", val->value);

    str.append_format(", {{{},{},{}}}", val->x, val->y, val->z);

    if (val->flags & VALID_ALL)
    {
      str.append(", valid=");
      if (val->flags & VALID_X)
        str.append('X');
      if (val->flags & VALID_Y)
        str.append('Y');
      if (val->flags & VALID_Z)
        str.append('Z');
    }

    // if (val->flags & VALID_TAINTED_Z)
    // str.append(", tainted");

    str.append(']');
  }
}

#endif

void CPU::PGXP::GTE_RTPS(float x, float y, float z, u32 value)
{
  PGXPValue& pvalue = PushSXY();
  pvalue.x = x;
  pvalue.y = y;
  pvalue.z = z;
  pvalue.value = value;
  pvalue.flags = VALID_ALL;

  if (g_settings.gpu_pgxp_vertex_cache)
    CacheVertex(value, pvalue);
}

bool CPU::PGXP::GTE_HasPreciseVertices(u32 sxy0, u32 sxy1, u32 sxy2)
{
  PGXPValue& SXY0 = GetSXY0();
  SXY0.Validate(sxy0);
  PGXPValue& SXY1 = GetSXY1();
  SXY1.Validate(sxy1);
  PGXPValue& SXY2 = GetSXY2();
  SXY2.Validate(sxy2);

  // Don't use accurate clipping for game-constructed values, which don't have a valid Z.
  return (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_XYZ) == VALID_XYZ));
}

float CPU::PGXP::GTE_NCLIP()
{
  const PGXPValue& SXY0 = GetSXY0();
  const PGXPValue& SXY1 = GetSXY1();
  const PGXPValue& SXY2 = GetSXY2();
  float nclip = ((SXY0.x * SXY1.y) + (SXY1.x * SXY2.y) + (SXY2.x * SXY0.y) - (SXY0.x * SXY2.y) - (SXY1.x * SXY0.y) -
                 (SXY2.x * SXY1.y));

  // ensure fractional values are not incorrectly rounded to 0
  const float nclip_abs = std::abs(nclip);
  if (0.1f < nclip_abs && nclip_abs < 1.0f)
    nclip += (nclip < 0.0f ? -1.0f : 1.0f);

  return nclip;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_MTC2(u32 reg, const PGXPValue& value, u32 val)
{
  switch (reg)
  {
    case 15:
    {
      // push FIFO
      PGXPValue& SXY2 = PushSXY();
      SXY2 = value;
      return;
    }

    // read-only registers
    case 29:
    case 31:
    {
      return;
    }

    default:
    {
      PGXPValue& gteVal = g_state.pgxp_gte[reg];
      gteVal = value;
      gteVal.value = val;
      return;
    }
  }
}

void CPU::PGXP::CPU_MFC2(Instruction instr, u32 rdVal)
{
  // CPU[Rt] = GTE_D[Rd]
  const u32 idx = instr.cop.Cop2Index();
  LOG_VALUES_1(CPU::GetGTERegisterName(idx), rdVal, &g_state.pgxp_gte[idx]);

  PGXPValue& prdVal = g_state.pgxp_gte[idx];
  prdVal.Validate(rdVal);
  SetRtValue(instr, prdVal, rdVal);
}

void CPU::PGXP::CPU_MTC2(Instruction instr, u32 rtVal)
{
  // GTE_D[Rd] = CPU[Rt]
  const u32 idx = instr.cop.Cop2Index();
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  CPU_MTC2(idx, prtVal, rtVal);
}

void CPU::PGXP::CPU_LWC2(Instruction instr, u32 addr, u32 rtVal)
{
  // GTE_D[Rt] = Mem[addr]
  LOG_VALUES_LOAD(addr, rtVal);

  const PGXPValue& pMem = ValidateAndLoadMem(addr, rtVal);
  CPU_MTC2(static_cast<u32>(instr.r.rt.GetValue()), pMem, rtVal);
}

void CPU::PGXP::CPU_SWC2(Instruction instr, u32 addr, u32 rtVal)
{
  //  Mem[addr] = GTE_D[Rt]
  const u32 idx = static_cast<u32>(instr.r.rt.GetValue());
  PGXPValue& prtVal = g_state.pgxp_gte[idx];
#ifdef LOG_VALUES
  LOG_VALUES_1(CPU::GetGTERegisterName(idx), rtVal, &prtVal);
  std::fprintf(s_log, " addr=%08X", addr);
#endif
  prtVal.Validate(rtVal);
  WriteMem(addr, prtVal);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CacheVertex(u32 value, const PGXPValue& vertex)
{
  const s16 sx = static_cast<s16>(value & 0xFFFFu);
  const s16 sy = static_cast<s16>(value >> 16);
  DebugAssert(sx >= -1024 && sx <= 1023 && sy >= -1024 && sy <= 1023);
  s_vertex_cache[(sy + 1024) * VERTEX_CACHE_WIDTH + (sx + 1024)] = vertex;
}

ALWAYS_INLINE_RELEASE CPU::PGXPValue* CPU::PGXP::GetCachedVertex(u32 value)
{
  const s16 sx = static_cast<s16>(value & 0xFFFFu);
  const s16 sy = static_cast<s16>(value >> 16);
  return (sx >= -1024 && sx <= 1023 && sy >= -1024 && sy <= 1013) ?
           &s_vertex_cache[(sy + 1024) * VERTEX_CACHE_WIDTH + (sx + 1024)] :
           nullptr;
}

ALWAYS_INLINE_RELEASE float CPU::PGXP::TruncateVertexPosition(float p)
{
  const s32 int_part = static_cast<s32>(p);
  const float int_part_f = static_cast<float>(int_part);
  return static_cast<float>(static_cast<s16>(int_part << 5) >> 5) + (p - int_part_f);
}

ALWAYS_INLINE_RELEASE bool CPU::PGXP::IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y)
{
  const float tolerance = g_settings.gpu_pgxp_tolerance;
  if (tolerance < 0.0f)
    return true;

  return (std::abs(precise_x - static_cast<float>(int_x)) <= tolerance &&
          std::abs(precise_y - static_cast<float>(int_y)) <= tolerance);
}

bool CPU::PGXP::GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y,
                                 float* out_w)
{
  const PGXPValue* vert = GetPtr(addr);
  if (vert && ((vert->flags & VALID_XY) == VALID_XY) && (vert->value == value))
  {
    // There is a value here with valid X and Y coordinates
    *out_x = TruncateVertexPosition(vert->x) + static_cast<float>(xOffs);
    *out_y = TruncateVertexPosition(vert->y) + static_cast<float>(yOffs);
    *out_w = vert->z / 32768.0f;

#ifdef LOG_LOOKUPS
    GL_INS_FMT("0x{:08X} {},{} => {},{} ({},{},{}) ({},{})", addr, x, y, *out_x, *out_y,
               TruncateVertexPosition(vert->x), TruncateVertexPosition(vert->y), vert->z, std::abs(*out_x - x),
               std::abs(*out_y - y));
#endif

    if (IsWithinTolerance(*out_x, *out_y, x, y))
    {
      // check validity of z component
      return ((vert->flags & VALID_Z) == VALID_Z);
    }
  }

  if (g_settings.gpu_pgxp_vertex_cache)
  {
    vert = GetCachedVertex(value);
    if (vert && (vert->flags & VALID_XY) == VALID_XY)
    {
      *out_x = TruncateVertexPosition(vert->x) + static_cast<float>(xOffs);
      *out_y = TruncateVertexPosition(vert->y) + static_cast<float>(yOffs);
      *out_w = vert->z / 32768.0f;

      if (IsWithinTolerance(*out_x, *out_y, x, y))
        return false;
    }
  }

  // no valid value can be found anywhere, use the native PSX data
  *out_x = static_cast<float>(x);
  *out_y = static_cast<float>(y);
  *out_w = 1.0f;
  return false;
}

void CPU::PGXP::CPU_LW(Instruction instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im]
  LOG_VALUES_LOAD(addr, rtVal);
  SetRtValue(instr, ValidateAndLoadMem(addr, rtVal));
}

void CPU::PGXP::CPU_LBx(Instruction instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_LOAD(addr, rtVal);
  SetRtValue(instr, INVALID_VALUE);
}

void CPU::PGXP::CPU_LH(Instruction instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im] (sign extended)
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndLoadMem16(GetRtValue(instr), addr, rtVal, true);
}

void CPU::PGXP::CPU_LHU(Instruction instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im] (zero extended)
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndLoadMem16(GetRtValue(instr), addr, rtVal, false);
}

void CPU::PGXP::CPU_SB(Instruction instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_STORE(instr.r.rt.GetValue(), rtVal, addr);
  WriteMem(addr, INVALID_VALUE);
}

void CPU::PGXP::CPU_SH(Instruction instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_STORE(instr.r.rt.GetValue(), rtVal, addr);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  WriteMem16(addr, prtVal);
}

void CPU::PGXP::CPU_SW(Instruction instr, u32 addr, u32 rtVal)
{
  // Mem[Rs + Im] = Rt
  LOG_VALUES_STORE(instr.r.rt.GetValue(), rtVal, addr);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  WriteMem(addr, prtVal);
}

void CPU::PGXP::CPU_MOVE_Packed(u32 rd_and_rs, u32 rsVal)
{
  const u32 Rs = (rd_and_rs & 0xFFu);
  const u32 Rd = (rd_and_rs >> 8);
  CPU_MOVE(Rd, Rs, rsVal);
}

void CPU::PGXP::CPU_MOVE(u32 Rd, u32 Rs, u32 rsVal)
{
#ifdef LOG_VALUES
  const Instruction instr = {0};
  LOG_VALUES_C1(Rs, rsVal);
#endif
  PGXPValue& prsVal = g_state.pgxp_gpr[Rs];
  prsVal.Validate(rsVal);
  g_state.pgxp_gpr[Rd] = prsVal;
}

void CPU::PGXP::CPU_ADDI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs + Imm (signed)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);

  const u32 immVal = instr.i.imm_sext32();

  PGXPValue& prtVal = GetRtValue(instr);
  prtVal = prsVal;

  if (immVal == 0)
    return;

  if (rsVal == 0)
  {
    // x is low precision value
    prtVal.x = static_cast<float>(LOWORD_S16(immVal));
    prtVal.y = static_cast<float>(HIWORD_S16(immVal));
    prtVal.flags |= VALID_X | VALID_Y | VALID_TAINTED_Z;
    prtVal.value = immVal;
    return;
  }

  prtVal.x = static_cast<float>(f16Unsign(prtVal.x));
  prtVal.x += static_cast<float>(LOWORD_U16(immVal));

  // carry on over/underflow
  const float of = (prtVal.x > USHRT_MAX) ? 1.0f : (prtVal.x < 0.0f) ? -1.0f : 0.0f;
  prtVal.x = static_cast<float>(f16Sign(prtVal.x));
  prtVal.y += HIWORD_S16(immVal) + of;

  // truncate on overflow/underflow
  prtVal.y += (prtVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prtVal.y < SHRT_MIN) ? (USHRT_MAX + 1) : 0.0f;

  prtVal.value = rsVal + immVal;

  prtVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_ANDI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs & Imm
  const u32 imm = instr.i.imm_zext32();
  const u32 rtVal = rsVal & imm;
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = GetRtValue(instr);

  // remove upper 16-bits
  prtVal.y = 0.0f;
  prtVal.z = prsVal.z;
  prtVal.value = rtVal;
  prtVal.flags = prsVal.flags | VALID_Y | VALID_TAINTED_Z;

  switch (imm)
  {
    case 0:
    {
      // if 0 then x == 0
      prtVal.x = 0.0f;
      prtVal.flags |= VALID_X;
    }
    break;

    case 0xFFFFu:
    {
      // if saturated then x == x
      prtVal.x = prsVal.x;
    }
    break;

    default:
    {
      // otherwise x is low precision value
      prtVal.x = static_cast<float>(LOWORD_S16(rtVal));
      prtVal.flags |= VALID_X;
    }
    break;
  }
}

void CPU::PGXP::CPU_ORI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs | Imm
  const u32 imm = instr.i.imm_zext32();
  const u32 rtVal = rsVal | imm;

  PGXPValue& pRsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& pRtVal = GetRtValue(instr);
  pRtVal = pRsVal;
  pRtVal.value = rtVal;

  if (imm == 0) [[unlikely]]
  {
    // if 0 then x == x
  }
  else
  {
    // otherwise x is low precision value
    pRtVal.x = static_cast<float>(LOWORD_S16(rtVal));
    pRtVal.flags |= VALID_X | VALID_TAINTED_Z;
  }
}

void CPU::PGXP::CPU_XORI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs ^ Imm
  const u32 imm = instr.i.imm_zext32();
  const u32 rtVal = rsVal ^ imm;

  PGXPValue& pRsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& pRtVal = GetRtValue(instr);
  pRtVal = pRsVal;
  pRtVal.value = rtVal;

  if (imm == 0) [[unlikely]]
  {
    // if 0 then x == x
  }
  else
  {
    // otherwise x is low precision value
    pRtVal.x = static_cast<float>(LOWORD_S16(rtVal));
    pRtVal.flags |= VALID_X | VALID_TAINTED_Z;
  }
}

void CPU::PGXP::CPU_SLTI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs < Imm (signed)
  const s32 imm = instr.i.imm_s16();
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);

  const float fimmx = static_cast<float>(imm);
  const float fimmy = fimmx < 0.0f ? -1.0f : 0.0f;

  PGXPValue& prtVal = GetRtValue(instr);
  prtVal.x = (prsVal.GetValidY(rsVal) < fimmy || prsVal.GetValidX(rsVal) < fimmx) ? 1.0f : 0.0f;
  prtVal.y = 0.0f;
  prtVal.z = prsVal.z;
  prtVal.flags = prsVal.flags | VALID_X | VALID_Y | VALID_TAINTED_Z;
  prtVal.value = BoolToUInt32(static_cast<s32>(rsVal) < imm);
}

void CPU::PGXP::CPU_SLTIU(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs < Imm (Unsigned)
  const u32 imm = instr.i.imm_u16();
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);

  const float fimmx = static_cast<float>(static_cast<s16>(imm)); // deliberately signed
  const float fimmy = fimmx < 0.0f ? -1.0f : 0.0f;

  PGXPValue& prtVal = GetRtValue(instr);
  prtVal.x =
    (f16Unsign(prsVal.GetValidY(rsVal)) < f16Unsign(fimmy) || f16Unsign(prsVal.GetValidX(rsVal)) < fimmx) ? 1.0f : 0.0f;
  prtVal.y = 0.0f;
  prtVal.z = prsVal.z;
  prtVal.flags = prsVal.flags | VALID_X | VALID_Y | VALID_TAINTED_Z;
  prtVal.value = BoolToUInt32(rsVal < imm);
}

void CPU::PGXP::CPU_LUI(Instruction instr)
{
  LOG_VALUES_NV();

  // Rt = Imm << 16
  PGXPValue& pRtVal = GetRtValue(instr);
  pRtVal.x = 0.0f;
  pRtVal.y = static_cast<float>(instr.i.imm_s16());
  pRtVal.z = 0.0f;
  pRtVal.value = instr.i.imm_zext32() << 16;
  pRtVal.flags = VALID_XY;
}

void CPU::PGXP::CPU_ADD(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs + Rt (signed)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXPValue& prdVal = GetRdValue(instr);

  if (rtVal == 0)
  {
    prdVal = prsVal;
    CopyZIfMissing(prdVal, prtVal);
  }
  else if (rsVal == 0)
  {
    prdVal = prtVal;
    CopyZIfMissing(prdVal, prsVal);
  }
  else
  {
    const double x = f16Unsign(prsVal.GetValidX(rsVal)) + f16Unsign(prtVal.GetValidX(rtVal));

    // carry on over/underflow
    const float of = (x > USHRT_MAX) ? 1.0f : (x < 0.0f) ? -1.0f : 0.0f;
    prdVal.x = static_cast<float>(f16Sign(x));
    prdVal.y = prsVal.GetValidY(rsVal) + prtVal.GetValidY(rtVal) + of;

    // truncate on overflow/underflow
    prdVal.y += (prdVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prdVal.y < SHRT_MIN) ? (USHRT_MAX + 1) : 0.0f;

    prdVal.value = rsVal + rtVal;

    // valid x/y only if one side had a valid x/y
    prdVal.flags = prsVal.flags | (prtVal.flags & VALID_XY) | VALID_TAINTED_Z;

    SelectZ(prdVal.z, prdVal.flags, prsVal, prtVal);
  }
}

void CPU::PGXP::CPU_SUB(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs - Rt (signed)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXPValue& prdVal = GetRdValue(instr);

  if (rtVal == 0)
  {
    prdVal = prsVal;
    CopyZIfMissing(prdVal, prtVal);
  }
  else
  {
    const double x = f16Unsign(prsVal.GetValidX(rsVal)) - f16Unsign(prtVal.GetValidX(rtVal));

    // carry on over/underflow
    const float of = (x > USHRT_MAX) ? 1.0f : (x < 0.0f) ? -1.0f : 0.0f;
    prdVal.x = static_cast<float>(f16Sign(x));
    prdVal.y = prsVal.GetValidY(rsVal) - (prtVal.GetValidY(rtVal) - of);

    // truncate on overflow/underflow
    prdVal.y += (prdVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prdVal.y < SHRT_MIN) ? (USHRT_MAX + 1) : 0.0f;

    prdVal.value = rsVal - rtVal;

    // valid x/y only if one side had a valid x/y
    prdVal.flags = prsVal.flags | (prtVal.flags & VALID_XY) | VALID_TAINTED_Z;

    SelectZ(prdVal.z, prdVal.flags, prsVal, prtVal);
  }
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_BITWISE(Instruction instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs & Rt
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);

  float x, y;
  if (LOWORD_U16(rdVal) == 0)
    x = 0.0f;
  else if (LOWORD_U16(rdVal) == LOWORD_U16(rsVal))
    x = prsVal.GetValidX(rsVal);
  else if (LOWORD_U16(rdVal) == LOWORD_U16(rtVal))
    x = prtVal.GetValidX(rtVal);
  else
    x = static_cast<float>(LOWORD_S16(rdVal));

  if (HIWORD_U16(rdVal) == 0)
    y = 0.0f;
  else if (HIWORD_U16(rdVal) == HIWORD_U16(rsVal))
    y = prsVal.GetValidY(rsVal);
  else if (HIWORD_U16(rdVal) == HIWORD_U16(rtVal))
    y = prtVal.GetValidY(rtVal);
  else
    y = static_cast<float>(HIWORD_S16(rdVal));

  // Why not write directly to prdVal? Because it might be the same as the source.
  u32 flags = ((prsVal.flags | prtVal.flags) & VALID_XY) ? (VALID_XY | VALID_TAINTED_Z) : 0;
  PGXPValue& prdVal = GetRdValue(instr);
  SelectZ(prdVal.z, flags, prsVal, prtVal);
  prdVal.x = x;
  prdVal.y = y;
  prdVal.flags = flags;
  prdVal.value = rdVal;
}

void CPU::PGXP::CPU_AND_(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs & Rt
  const u32 rdVal = rsVal & rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_OR_(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs | Rt
  const u32 rdVal = rsVal | rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_XOR_(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs ^ Rt
  const u32 rdVal = rsVal ^ rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_NOR(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs NOR Rt
  const u32 rdVal = ~(rsVal | rtVal);
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_SLT(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs < Rt (signed)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXPValue& prdVal = GetRdValue(instr);
  prdVal.x = (prsVal.GetValidY(rsVal) < prtVal.GetValidY(rtVal) ||
              f16Unsign(prsVal.GetValidX(rsVal)) < f16Unsign(prtVal.GetValidX(rtVal))) ?
               1.0f :
               0.0f;
  prdVal.y = 0.0f;
  prdVal.z = prsVal.z;
  prdVal.flags = prsVal.flags | VALID_TAINTED_Z | VALID_X | VALID_Y;
  prdVal.value = BoolToUInt32(static_cast<s32>(rsVal) < static_cast<s32>(rtVal));
}

void CPU::PGXP::CPU_SLTU(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs < Rt (unsigned)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXPValue& prdVal = GetRdValue(instr);
  prdVal.x = (f16Unsign(prsVal.GetValidY(rsVal)) < f16Unsign(prtVal.GetValidY(rtVal)) ||
              f16Unsign(prsVal.GetValidX(rsVal)) < f16Unsign(prtVal.GetValidX(rtVal))) ?
               1.0f :
               0.0f;
  prdVal.y = 0.0f;
  prdVal.z = prsVal.z;
  prdVal.flags = prsVal.flags | VALID_TAINTED_Z | VALID_X | VALID_Y;
  prdVal.value = BoolToUInt32(rsVal < rtVal);
}

void CPU::PGXP::CPU_MULT(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Hi/Lo = Rs * Rt (signed)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXPValue& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXPValue& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  const float rsx = prsVal.GetValidX(rsVal);
  const float rsy = prsVal.GetValidY(rsVal);
  const float rtx = prtVal.GetValidX(rtVal);
  const float rty = prtVal.GetValidY(rtVal);

  // Multiply out components
  const double xx = f16Unsign(rsx) * f16Unsign(rtx);
  const double xy = f16Unsign(rsx) * (rty);
  const double yx = rsy * f16Unsign(rtx);
  const double yy = rsy * rty;

  // Split values into outputs
  const double lx = xx;
  const double ly = f16Overflow(xx) + (xy + yx);
  const double hx = f16Overflow(ly) + yy;
  const double hy = f16Overflow(hx);

  ploVal.x = static_cast<float>(f16Sign(lx));
  ploVal.y = static_cast<float>(f16Sign(ly));
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);
  phiVal.x = static_cast<float>(f16Sign(hx));
  phiVal.y = static_cast<float>(f16Sign(hy));
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  // compute PSX value
  const u64 result = static_cast<u64>(static_cast<s64>(SignExtend64(rsVal)) * static_cast<s64>(SignExtend64(rtVal)));
  phiVal.value = Truncate32(result >> 32);
  ploVal.value = Truncate32(result);
}

void CPU::PGXP::CPU_MULTU(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Hi/Lo = Rs * Rt (unsigned)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXPValue& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXPValue& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  const float rsx = prsVal.GetValidX(rsVal);
  const float rsy = prsVal.GetValidY(rsVal);
  const float rtx = prtVal.GetValidX(rtVal);
  const float rty = prtVal.GetValidY(rtVal);

  // Multiply out components
  const double xx = f16Unsign(rsx) * f16Unsign(rtx);
  const double xy = f16Unsign(rsx) * f16Unsign(rty);
  const double yx = f16Unsign(rsy) * f16Unsign(rtx);
  const double yy = f16Unsign(rsy) * f16Unsign(rty);

  // Split values into outputs
  const double lx = xx;
  const double ly = f16Overflow(xx) + (xy + yx);
  const double hx = f16Overflow(ly) + yy;
  const double hy = f16Overflow(hx);

  ploVal.x = static_cast<float>(f16Sign(lx));
  ploVal.y = static_cast<float>(f16Sign(ly));
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);
  phiVal.x = static_cast<float>(f16Sign(hx));
  phiVal.y = static_cast<float>(f16Sign(hy));
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  // compute PSX value
  const u64 result = ZeroExtend64(rsVal) * ZeroExtend64(rtVal);
  phiVal.value = Truncate32(result >> 32);
  ploVal.value = Truncate32(result);
}

void CPU::PGXP::CPU_DIV(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Lo = Rs / Rt (signed)
  // Hi = Rs % Rt (signed)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXPValue& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXPValue& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  const double vs = f16Unsign(prsVal.GetValidX(rsVal)) + prsVal.GetValidY(rsVal) * static_cast<double>(1 << 16);
  const double vt = f16Unsign(prtVal.GetValidX(rtVal)) + prtVal.GetValidY(rtVal) * static_cast<double>(1 << 16);

  const double lo = vs / vt;
  ploVal.y = static_cast<float>(f16Sign(f16Overflow(lo)));
  ploVal.x = static_cast<float>(f16Sign(lo));
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  const double hi = std::fmod(vs, vt);
  phiVal.y = static_cast<float>(f16Sign(f16Overflow(hi)));
  phiVal.x = static_cast<float>(f16Sign(hi));
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  // compute PSX value
  if (static_cast<s32>(rtVal) == 0)
  {
    // divide by zero
    ploVal.value = (static_cast<s32>(rsVal) >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
    phiVal.value = static_cast<u32>(static_cast<s32>(rsVal));
  }
  else if (rsVal == UINT32_C(0x80000000) && static_cast<s32>(rtVal) == -1)
  {
    // unrepresentable
    ploVal.value = UINT32_C(0x80000000);
    phiVal.value = 0;
  }
  else
  {
    ploVal.value = static_cast<u32>(static_cast<s32>(rsVal) / static_cast<s32>(rtVal));
    phiVal.value = static_cast<u32>(static_cast<s32>(rsVal) % static_cast<s32>(rtVal));
  }
}

void CPU::PGXP::CPU_DIVU(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Lo = Rs / Rt (unsigned)
  // Hi = Rs % Rt (unsigned)
  PGXPValue& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXPValue& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXPValue& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  const double vs =
    f16Unsign(prsVal.GetValidX(rsVal)) + f16Unsign(prsVal.GetValidY(rsVal)) * static_cast<double>(1 << 16);
  const double vt =
    f16Unsign(prtVal.GetValidX(rtVal)) + f16Unsign(prtVal.GetValidY(rtVal)) * static_cast<double>(1 << 16);

  const double lo = vs / vt;
  ploVal.y = static_cast<float>(f16Sign(f16Overflow(lo)));
  ploVal.x = static_cast<float>(f16Sign(lo));
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  const double hi = std::fmod(vs, vt);
  phiVal.y = static_cast<float>(f16Sign(f16Overflow(hi)));
  phiVal.x = static_cast<float>(f16Sign(hi));
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  if (rtVal == 0)
  {
    // divide by zero
    ploVal.value = UINT32_C(0xFFFFFFFF);
    phiVal.value = rsVal;
  }
  else
  {
    ploVal.value = rsVal / rtVal;
    phiVal.value = rsVal % rtVal;
  }
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_SLL(Instruction instr, u32 rtVal, u32 sh)
{
  const u32 rdVal = rtVal << sh;
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXPValue& prdVal = GetRdValue(instr);
  prdVal.z = prtVal.z;
  prdVal.value = rdVal;

  if (sh >= 32) [[unlikely]]
  {
    prdVal.x = 0.0f;
    prdVal.y = 0.0f;
    prdVal.flags = prtVal.flags | VALID_XY | VALID_TAINTED_Z;
  }
  else if (sh == 16)
  {
    prdVal.y = prtVal.x;
    prdVal.x = 0.0f;

    // Only set valid X if there's also a valid Y. We could use GetValidX() to pull it from the low precision value
    // instead, need to investigate further. Spyro breaks if only X is set even if Y is not valid.
    // prdVal.flags = (prtVal.flags & ~VALID_Y) | ((prtVal.flags & VALID_X) << 1) | VALID_X | VALID_TAINTED_Z;
    prdVal.flags = (prtVal.flags | VALID_TAINTED_Z) | ((prtVal.flags & VALID_Y) >> 1);
  }
  else if (sh >= 16)
  {
    prdVal.y = static_cast<float>(f16Sign(f16Unsign(prtVal.x * static_cast<double>(1 << (sh - 16)))));
    prdVal.x = 0.0f;

    // See above.
    // prdVal.flags = (prtVal.flags & ~VALID_Y) | ((prtVal.flags & VALID_X) << 1) | VALID_X | VALID_TAINTED_Z;
    prdVal.flags = (prtVal.flags | VALID_TAINTED_Z) | ((prtVal.flags & VALID_Y) >> 1);
  }
  else
  {
    const double x = f16Unsign(prtVal.x) * static_cast<double>(1 << sh);
    const double y = (f16Unsign(prtVal.y) * static_cast<double>(1 << sh)) + f16Overflow(x);
    prdVal.x = static_cast<float>(f16Sign(x));
    prdVal.y = static_cast<float>(f16Sign(y));
    prdVal.flags = (prtVal.flags | VALID_TAINTED_Z);
  }
}

void CPU::PGXP::CPU_SLL(Instruction instr, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // Rd = Rt << Sa
  const u32 sh = instr.r.shamt;
  CPU_SLL(instr, rtVal, sh);
}

void CPU::PGXP::CPU_SLLV(Instruction instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(instr.r.rt.GetValue(), rtVal, instr.r.rs.GetValue(), rsVal);

  // Rd = Rt << Rs
  const u32 sh = rsVal & 0x1F;
  CPU_SLL(instr, rtVal, sh);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_SRx(Instruction instr, u32 rtVal, u32 sh, bool sign, bool is_variable)
{
  const u32 rdVal = sign ? static_cast<u32>(static_cast<s32>(rtVal) >> sh) : (rtVal >> sh);
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);

  double x = prtVal.x;
  double y = sign ? prtVal.y : f16Unsign(prtVal.y);

  const u32 iX = SignExtend32(LOWORD_S16(rtVal));   // remove Y
  const u32 iY = SET_LOWORD(rtVal, HIWORD_U16(iX)); // overwrite x with sign(x)

  // Shift test values
  const u32 dX = static_cast<u32>(static_cast<s32>(iX) >> sh);
  const u32 dY = sign ? static_cast<u32>(static_cast<s32>(iY) >> sh) : (iY >> sh);

  if (LOWORD_S16(dX) != HIWORD_S16(iX))
    x = x / static_cast<double>(1 << sh);
  else
    x = LOWORD_S16(dX); // only sign bits left

  if (LOWORD_S16(dY) != HIWORD_S16(iX))
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * static_cast<double>(1 << (16 - sh));
      if (prtVal.x < 0)
        x += static_cast<double>(1 << (16 - sh));
    }
    else
    {
      x += y / static_cast<double>(1 << (sh - 16));
    }
  }

  if ((HIWORD_S16(dY) == 0) || (HIWORD_S16(dY) == -1))
    y = HIWORD_S16(dY);
  else
    y = y / static_cast<double>(1 << sh);

  PGXPValue& prdVal = GetRdValue(instr);

  // Use low precision/rounded values when we're not shifting an entire component,
  // and it's not originally from a 3D value. Too many false positives in P2/etc.
  // What we probably should do is not set the valid flag on non-3D values to begin
  // with, only letting them become valid when used in another expression.
  if (sign && !is_variable && !(prtVal.flags & VALID_Z) && sh < 16)
  {
    prdVal.x = static_cast<float>(LOWORD_S16(rdVal));
    prdVal.y = static_cast<float>(HIWORD_S16(rdVal));
    prdVal.z = 0.0f;
    prdVal.value = rdVal;
    prdVal.flags = VALID_XY | VALID_TAINTED_Z;
  }
  else
  {
    prdVal.x = static_cast<float>(f16Sign(x));
    prdVal.y = static_cast<float>(f16Sign(y));
    prdVal.z = prtVal.z;
    prdVal.value = rdVal;
    prdVal.flags = prtVal.flags | VALID_TAINTED_Z;
  }
}

void CPU::PGXP::CPU_SRL(Instruction instr, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // Rd = Rt >> Sa
  const u32 sh = instr.r.shamt;
  CPU_SRx(instr, rtVal, sh, false, false);
}

void CPU::PGXP::CPU_SRLV(Instruction instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(instr.r.rt.GetValue(), rtVal, instr.r.rs.GetValue(), rsVal);

  // Rd = Rt >> Sa
  const u32 sh = rsVal & 0x1F;
  CPU_SRx(instr, rtVal, sh, false, true);
}

void CPU::PGXP::CPU_SRA(Instruction instr, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // Rd = Rt >> Sa
  const u32 sh = instr.r.shamt;
  CPU_SRx(instr, rtVal, sh, true, false);
}

void CPU::PGXP::CPU_SRAV(Instruction instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(instr.r.rt.GetValue(), rtVal, instr.r.rs.GetValue(), rsVal);

  // Rd = Rt >> Sa
  const u32 sh = rsVal & 0x1F;
  CPU_SRx(instr, rtVal, sh, true, true);
}

void CPU::PGXP::CPU_MFC0(Instruction instr, u32 rdVal)
{
  const u32 idx = static_cast<u8>(instr.r.rd.GetValue());
  LOG_VALUES_1(TinyString::from_format("cop0_{}", idx).c_str(), rdVal, &g_state.pgxp_cop0[idx]);

  // CPU[Rt] = CP0[Rd]
  PGXPValue& prdVal = g_state.pgxp_cop0[idx];
  prdVal.Validate(rdVal);

  PGXPValue& prtVal = GetRtValue(instr);
  prtVal = prdVal;
  prtVal.value = rdVal;
}

void CPU::PGXP::CPU_MTC0(Instruction instr, u32 rdVal, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // CP0[Rd] = CPU[Rt]
  PGXPValue& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXPValue& prdVal = g_state.pgxp_cop0[static_cast<u8>(instr.r.rd.GetValue())];
  prdVal = prtVal;
  prtVal.value = rdVal;
}
