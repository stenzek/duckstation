// SPDX-FileCopyrightText: 2016 iCatButler, 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: GPL-2.0+

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

// TODO: Get rid of all the rs/rt subscripting.

namespace CPU::PGXP {
namespace {

enum : u32
{
  VERTEX_CACHE_WIDTH = 0x800 * 2,
  VERTEX_CACHE_HEIGHT = 0x800 * 2,
  VERTEX_CACHE_SIZE = VERTEX_CACHE_WIDTH * VERTEX_CACHE_HEIGHT,
  PGXP_MEM_SIZE = (static_cast<u32>(Bus::RAM_8MB_SIZE) + static_cast<u32>(CPU::SCRATCHPAD_SIZE)) / 4,
  PGXP_MEM_SCRATCH_OFFSET = Bus::RAM_8MB_SIZE / 4,
};

enum : u32
{
  COMP_X,
  COMP_Y,
  COMP_Z,
};

enum : u32
{
  ALL = 0xFFFFFFFFu,
  VALID_X = (1u << 0),
  VALID_Y = (1u << 1),
  VALID_Z = (1u << 2),
  VALID_XY = (VALID_X | VALID_Y),
  VALID_XYZ = (VALID_X | VALID_Y | VALID_Z),
  VALID_ALL = (VALID_X | VALID_Y | VALID_Z),
  INV_VALID_ALL = (ALL ^ VALID_ALL),
};

union psx_value
{
  u32 d;
  s32 sd;
  struct
  {
    u16 l, h;
  } w;
  struct
  {
    s16 l, h;
  } sw;
};
} // namespace

static void CacheVertex(s16 sx, s16 sy, const PGXP_value& vertex);
static PGXP_value* GetCachedVertex(short sx, short sy);

static float TruncateVertexPosition(float p);
static bool IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y);

static void MakeValid(PGXP_value* pV, u32 psxV);

static void Validate(PGXP_value* pV, u32 psxV);
static void MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask);

static double f16Sign(double in);
static double f16Unsign(double in);
static double f16Overflow(double in);

static PGXP_value* GetPtr(u32 addr);

static void ValidateAndCopyMem(PGXP_value* dest, u32 addr, u32 value);
static void ValidateAndCopyMem16(PGXP_value* dest, u32 addr, u32 value, bool sign);

static void CPU_MTC2_int(const PGXP_value& value, u32 reg);
static void CPU_BITWISE(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal);

static void WriteMem(const PGXP_value* value, u32 addr);
static void WriteMem16(const PGXP_value* src, u32 addr);

static void CopyZIfMissing(PGXP_value& dst, const PGXP_value& src);
static void SelectZ(PGXP_value& dst, const PGXP_value& src1, const PGXP_value& src2);

#ifdef LOG_VALUES
static void LogInstruction(u32 pc, u32 instr);
static void LogValue(const char* name, u32 rval, const PGXP_value* val);
static void LogValueStr(SmallStringBase& str, const char* name, u32 rval, const PGXP_value* val);

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

static constexpr PGXP_value PGXP_value_invalid = {0.f, 0.f, 0.f, 0, 0};
static constexpr PGXP_value PGXP_value_zero = {0.f, 0.f, 0.f, 0, VALID_ALL};

static PGXP_value* s_mem = nullptr;
static PGXP_value* s_vertex_cache = nullptr;

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
    s_mem = static_cast<PGXP_value*>(std::calloc(PGXP_MEM_SIZE, sizeof(PGXP_value)));
    if (!s_mem)
      Panic("Failed to allocate PGXP memory");
  }

  if (g_settings.gpu_pgxp_vertex_cache && !s_vertex_cache)
  {
    s_vertex_cache = static_cast<PGXP_value*>(std::calloc(VERTEX_CACHE_SIZE, sizeof(PGXP_value)));
    if (!s_vertex_cache)
    {
      Log_ErrorPrint("Failed to allocate memory for vertex cache, disabling.");
      g_settings.gpu_pgxp_vertex_cache = false;
    }
  }

  if (s_vertex_cache)
    std::memset(s_vertex_cache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);
}

void CPU::PGXP::Reset()
{
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));

  if (s_mem)
    std::memset(s_mem, 0, sizeof(PGXP_value) * PGXP_MEM_SIZE);

  if (s_vertex_cache)
    std::memset(s_vertex_cache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);
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

// Instruction register decoding
#define op(_instr) (_instr >> 26)          // The op part of the instruction register
#define func(_instr) ((_instr) & 0x3F)     // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)  // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)      // The immediate part of the instruction register
#define cop2idx(_instr) (((_instr >> 11) & 0x1F) | ((_instr >> 17) & 0x20))

#define SX0 (g_state.pgxp_gte[12].x)
#define SY0 (g_state.pgxp_gte[12].y)
#define SX1 (g_state.pgxp_gte[13].x)
#define SY1 (g_state.pgxp_gte[13].y)
#define SX2 (g_state.pgxp_gte[14].x)
#define SY2 (g_state.pgxp_gte[14].y)

#define SXY0 (g_state.pgxp_gte[12])
#define SXY1 (g_state.pgxp_gte[13])
#define SXY2 (g_state.pgxp_gte[14])
#define SXYP (g_state.pgxp_gte[15])

ALWAYS_INLINE_RELEASE void CPU::PGXP::MakeValid(PGXP_value* pV, u32 psxV)
{
  if ((pV->flags & VALID_XY) == VALID_XY)
    return;

  pV->x = static_cast<float>(static_cast<s16>(Truncate16(psxV)));
  pV->y = static_cast<float>(static_cast<s16>(Truncate16(psxV >> 16)));
  pV->z = 0.0f;
  pV->flags = VALID_XY;
  pV->value = psxV;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::Validate(PGXP_value* pV, u32 psxV)
{
  pV->flags &= (pV->value == psxV) ? ALL : INV_VALID_ALL;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask)
{
  pV->flags &= ((pV->value & mask) == (psxV & mask)) ? ALL : (ALL ^ (validMask));
}

ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Sign(double in)
{
  const s32 s = static_cast<s32>(static_cast<s64>(in * (USHRT_MAX + 1)));
  return static_cast<double>(s) / static_cast<double>(USHRT_MAX + 1);
}
ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Unsign(double in)
{
  return (in >= 0) ? in : (in + (USHRT_MAX + 1));
}
ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Overflow(double in)
{
  double out = 0;
  s64 v = ((s64)in) >> 16;
  out = (double)v;
  return out;
}

ALWAYS_INLINE_RELEASE CPU::PGXP_value* CPU::PGXP::GetPtr(u32 addr)
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

ALWAYS_INLINE_RELEASE void CPU::PGXP::ValidateAndCopyMem(PGXP_value* dest, u32 addr, u32 value)
{
  PGXP_value* pMem = GetPtr(addr);
  if (!pMem)
  {
    *dest = PGXP_value_invalid;
    return;
  }

  Validate(pMem, value);
  *dest = *pMem;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::ValidateAndCopyMem16(PGXP_value* dest, u32 addr, u32 value, bool sign)
{
  PGXP_value* pMem = GetPtr(addr);
  if (!pMem)
  {
    *dest = PGXP_value_invalid;
    return;
  }

  psx_value val{0}, mask{0};
  u32 valid_mask = 0;

  // determine if high or low word
  const bool hiword = ((addr & 2) != 0);
  if (hiword)
  {
    val.w.h = static_cast<u16>(value);
    mask.w.h = 0xFFFF;
    valid_mask = VALID_Y;
  }
  else
  {
    val.w.l = static_cast<u16>(value);
    mask.w.l = 0xFFFF;
    valid_mask = VALID_X;
  }

  // validate and copy whole value
  MaskValidate(pMem, val.d, mask.d, valid_mask);
  *dest = *pMem;

  // if high word then shift
  if (hiword)
  {
    dest->x = dest->y;
    dest->SetValid(COMP_X, dest->HasValid(COMP_Y));
  }

  // only set y as valid if x is also valid.. don't want to make fake values
  if (dest->HasValid(COMP_X))
  {
    dest->y = (dest->x < 0) ? -1.f * sign : 0.f;
    dest->SetValid(COMP_Y);
  }
  else
  {
    dest->y = 0.0f;
    dest->SetValid(COMP_Y, false);
  }

  dest->value = value;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::WriteMem(const PGXP_value* value, u32 addr)
{
  PGXP_value* pMem = GetPtr(addr);

  if (pMem)
    *pMem = *value;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::WriteMem16(const PGXP_value* src, u32 addr)
{
  PGXP_value* dest = GetPtr(addr);
  if (!dest)
    return;

  // determine if high or low word
  const bool hiword = ((addr & 2) != 0);
  if (hiword)
  {
    dest->y = src->x;
    dest->SetValid(COMP_Y, src->HasValid(COMP_X));
    dest->value = (dest->value & UINT32_C(0x0000FFFF)) | (src->value << 16);
  }
  else
  {
    dest->x = src->x;
    dest->SetValid(COMP_X, src->HasValid(COMP_X));
    dest->value = (dest->value & UINT32_C(0xFFFF0000)) | (src->value & UINT32_C(0x0000FFFF));
  }

  // overwrite z/w if valid
  // TODO: Check modified
  if (src->HasValid(COMP_Z))
  {
    dest->z = src->z;
    dest->SetValid(COMP_Z);
  }
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CopyZIfMissing(PGXP_value& dst, const PGXP_value& src)
{
  if (dst.HasValid(COMP_Z))
    return;

  dst.z = src.z;
  dst.flags |= (src.flags & VALID_Z);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::SelectZ(PGXP_value& dst, const PGXP_value& src1, const PGXP_value& src2)
{
  dst.z = src1.HasValid(COMP_Z) ? src1.z : src2.z;
  dst.flags |= ((src1.flags | src2.flags) & VALID_Z);
}

#ifdef LOG_VALUES
void CPU::PGXP::LogInstruction(u32 pc, u32 instr)
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
  DisassembleInstruction(&str, pc, instr);
  std::fprintf(s_log, "%08X %08X %-20s", pc, instr, str.c_str());
}

void CPU::PGXP::LogValue(const char* name, u32 rval, const PGXP_value* val)
{
  if (!s_log) [[unlikely]]
    return;

  SmallString str;
  LogValueStr(str, name, rval, val);
  std::fprintf(s_log, " %s", str.c_str());
}

void CPU::PGXP::LogValueStr(SmallStringBase& str, const char* name, u32 rval, const PGXP_value* val)
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

    if (val->flags != 0)
    {
      str.append(", valid=");
      if (val->flags & VALID_X)
        str.append('X');
      if (val->flags & VALID_Y)
        str.append('Y');
      if (val->flags & VALID_Z)
        str.append('Z');
    }

    str.append(']');
  }
}

#endif

void CPU::PGXP::GTE_PushSXYZ2f(float x, float y, float z, u32 v)
{
  // push values down FIFO
  SXY0 = SXY1;
  SXY1 = SXY2;

  SXY2.x = x;
  SXY2.y = y;
  SXY2.z = z;
  SXY2.value = v;
  SXY2.flags = VALID_ALL;

  if (g_settings.gpu_pgxp_vertex_cache)
    CacheVertex(static_cast<s16>(Truncate16(v)), static_cast<s16>(Truncate16(v >> 16)), SXY2);
}

#define VX(n) (psxRegs.CP2D.p[n << 1].sw.l)
#define VY(n) (psxRegs.CP2D.p[n << 1].sw.h)
#define VZ(n) (psxRegs.CP2D.p[(n << 1) + 1].sw.l)

int CPU::PGXP::GTE_NCLIP_valid(u32 sxy0, u32 sxy1, u32 sxy2)
{
  Validate(&SXY0, sxy0);
  Validate(&SXY1, sxy1);
  Validate(&SXY2, sxy2);

  // Don't use accurate clipping for game-constructed values, which don't have a valid Z.
  return (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_XYZ) == VALID_XYZ));
}

float CPU::PGXP::GTE_NCLIP()
{
  float nclip = ((SX0 * SY1) + (SX1 * SY2) + (SX2 * SY0) - (SX0 * SY2) - (SX1 * SY0) - (SX2 * SY1));

  // ensure fractional values are not incorrectly rounded to 0
  float nclipAbs = std::abs(nclip);
  if ((0.1f < nclipAbs) && (nclipAbs < 1.f))
    nclip += (nclip < 0.f ? -1 : 1);

  // float AX = SX1 - SX0;
  // float AY = SY1 - SY0;

  // float BX = SX2 - SX0;
  // float BY = SY2 - SY0;

  //// normalise A and B
  // float mA = sqrt((AX*AX) + (AY*AY));
  // float mB = sqrt((BX*BX) + (BY*BY));

  //// calculate AxB to get Z component of C
  // float CZ = ((AX * BY) - (AY * BX)) * (1 << 12);

  return nclip;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_MTC2_int(const PGXP_value& value, u32 reg)
{
  switch (reg)
  {
    case 15:
      // push FIFO
      SXY0 = SXY1;
      SXY1 = SXY2;
      SXY2 = value;
      SXYP = SXY2;
      break;

    case 31:
      return;
  }

  g_state.pgxp_gte[reg] = value;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void CPU::PGXP::CPU_MFC2(u32 instr, u32 rdVal)
{
  // CPU[Rt] = GTE_D[Rd]
  const u32 idx = cop2idx(instr);
  LOG_VALUES_1(CPU::GetGTERegisterName(idx), rdVal, &g_state.pgxp_gte[idx]);
  Validate(&g_state.pgxp_gte[idx], rdVal);
  g_state.pgxp_gpr[rt(instr)] = g_state.pgxp_gte[idx];
  g_state.pgxp_gpr[rt(instr)].value = rdVal;
}

void CPU::PGXP::CPU_MTC2(u32 instr, u32 rtVal)
{
  // GTE_D[Rd] = CPU[Rt]
  const u32 idx = cop2idx(instr);
  LOG_VALUES_C1(rt(instr), rtVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);
  CPU_MTC2_int(g_state.pgxp_gpr[rt(instr)], idx);
  g_state.pgxp_gte[idx].value = rtVal;
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void CPU::PGXP::CPU_LWC2(u32 instr, u32 addr, u32 rtVal)
{
  // GTE_D[Rt] = Mem[addr]
  LOG_VALUES_LOAD(addr, rtVal);
  PGXP_value val;
  ValidateAndCopyMem(&val, addr, rtVal);
  CPU_MTC2_int(val, rt(instr));
}

void CPU::PGXP::CPU_SWC2(u32 instr, u32 addr, u32 rtVal)
{
  //  Mem[addr] = GTE_D[Rt]
  const u32 idx = rt(instr);
#ifdef LOG_VALUES
  LOG_VALUES_1(CPU::GetGTERegisterName(idx), rtVal, &g_state.pgxp_gte[idx]);
  std::fprintf(s_log, " addr=%08X", addr);
#endif
  Validate(&g_state.pgxp_gte[idx], rtVal);
  WriteMem(&g_state.pgxp_gte[idx], addr);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CacheVertex(s16 sx, s16 sy, const PGXP_value& vertex)
{
  if (sx >= -0x800 && sx <= 0x7ff && sy >= -0x800 && sy <= 0x7ff)
  {
    // Write vertex into cache
    s_vertex_cache[(sy + 0x800) * VERTEX_CACHE_WIDTH + (sx + 0x800)] = vertex;
  }
}

ALWAYS_INLINE_RELEASE CPU::PGXP_value* CPU::PGXP::GetCachedVertex(short sx, short sy)
{
  if (sx >= -0x800 && sx <= 0x7ff && sy >= -0x800 && sy <= 0x7ff)
  {
    // Return pointer to cache entry
    return &s_vertex_cache[(sy + 0x800) * VERTEX_CACHE_WIDTH + (sx + 0x800)];
  }

  return nullptr;
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
  const PGXP_value* vert = GetPtr(addr);
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
    const short psx_x = (short)(value & 0xFFFFu);
    const short psx_y = (short)(value >> 16);

    // Look in cache for valid vertex
    vert = GetCachedVertex(psx_x, psx_y);
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

// Instruction register decoding
#define op(_instr) (_instr >> 26)          // The op part of the instruction register
#define func(_instr) ((_instr) & 0x3F)     // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)  // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)      // The immediate part of the instruction register
#define imm_sext(_instr)                                                                                               \
  static_cast<s32>(static_cast<s16>(_instr & 0xFFFF)) // The immediate part of the instruction register

void CPU::PGXP::CPU_LW(u32 instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im]
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndCopyMem(&g_state.pgxp_gpr[rt(instr)], addr, rtVal);
}

void CPU::PGXP::CPU_LBx(u32 instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_LOAD(addr, rtVal);
  g_state.pgxp_gpr[rt(instr)] = PGXP_value_invalid;
}

void CPU::PGXP::CPU_LH(u32 instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im] (sign extended)
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndCopyMem16(&g_state.pgxp_gpr[rt(instr)], addr, rtVal, true);
}

void CPU::PGXP::CPU_LHU(u32 instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im] (zero extended)
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndCopyMem16(&g_state.pgxp_gpr[rt(instr)], addr, rtVal, false);
}

void CPU::PGXP::CPU_SB(u32 instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_STORE(rt(instr), rtVal, addr);
  WriteMem(&PGXP_value_invalid, addr);
}

void CPU::PGXP::CPU_SH(u32 instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_STORE(rt(instr), rtVal, addr);
  PGXP_value* val = &g_state.pgxp_gpr[rt(instr)];
  Validate(val, rtVal);
  WriteMem16(val, addr);
}

void CPU::PGXP::CPU_SW(u32 instr, u32 addr, u32 rtVal)
{
  // Mem[Rs + Im] = Rt
  LOG_VALUES_STORE(rt(instr), rtVal, addr);
  PGXP_value* val = &g_state.pgxp_gpr[rt(instr)];
  Validate(val, rtVal);
  WriteMem(val, addr);
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
  const u32 instr = 0;
  LOG_VALUES_C1(Rs, rsVal);
#endif
  Validate(&g_state.pgxp_gpr[Rs], rsVal);
  g_state.pgxp_gpr[Rd] = g_state.pgxp_gpr[Rs];
}

void CPU::PGXP::CPU_ADDI(u32 instr, u32 rsVal)
{
  LOG_VALUES_C1(rs(instr), rsVal);

  // Rt = Rs + Imm (signed)
  PGXP_value& prsVal = g_state.pgxp_gpr[rs(instr)];
  Validate(&prsVal, rsVal);

  psx_value tempImm;
  tempImm.d = SignExtend32(static_cast<u16>(imm(instr)));

  PGXP_value& prtVal = g_state.pgxp_gpr[rt(instr)];
  prtVal = prsVal;

  if (tempImm.d == 0)
    return;

  prtVal.x = (float)f16Unsign(prtVal.x);
  prtVal.x += (float)tempImm.w.l;

  // carry on over/underflow
  float of = (prtVal.x > USHRT_MAX) ? 1.f : (prtVal.x < 0) ? -1.f : 0.f;
  prtVal.x = (float)f16Sign(prtVal.x);
  // ret.x -= of * (USHRT_MAX + 1);
  prtVal.y += tempImm.sw.h + of;

  // truncate on overflow/underflow
  prtVal.y += (prtVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prtVal.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

  prtVal.value = rsVal + tempImm.d;
}

void CPU::PGXP::CPU_ANDI(u32 instr, u32 rsVal)
{
  LOG_VALUES_C1(rs(instr), rsVal);

  // Rt = Rs & Imm
  const u32 rtVal = rsVal & imm(instr);
  PGXP_value& prsVal = g_state.pgxp_gpr[rs(instr)];
  Validate(&prsVal, rsVal);

  psx_value vRt;
  vRt.d = rtVal;

  PGXP_value& prtVal = g_state.pgxp_gpr[rt(instr)];
  prtVal = prsVal;

  prtVal.value = rtVal;
  prtVal.y = 0.f; // remove upper 16-bits
  prtVal.SetValid(COMP_Y);

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == 0
      // TODO: x should be valid here
      prtVal.x = 0.f;
      break;
    case 0xFFFF:
      // if saturated then x == x
      break;
    default:
      // otherwise x is low precision value
      prtVal.x = vRt.sw.l;
      prtVal.SetValid(COMP_X);
      break;
  }
}

void CPU::PGXP::CPU_ORI(u32 instr, u32 rsVal)
{
  LOG_VALUES_C1(rs(instr), rsVal);

  // Rt = Rs | Imm
  const u32 rtVal = rsVal | imm(instr);

  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];

  psx_value vRt;
  vRt.d = rtVal;

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      ret.x = vRt.sw.l;
      ret.SetValid(COMP_X);
      break;
  }

  ret.value = rtVal;
  g_state.pgxp_gpr[rt(instr)] = ret;
}

void CPU::PGXP::CPU_XORI(u32 instr, u32 rsVal)
{
  LOG_VALUES_C1(rs(instr), rsVal);

  // Rt = Rs ^ Imm
  const u32 rtVal = rsVal ^ imm(instr);

  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];

  psx_value vRt;
  vRt.d = rtVal;

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      ret.x = vRt.sw.l;
      ret.SetValid(COMP_X);
      break;
  }

  ret.value = rtVal;
  g_state.pgxp_gpr[rt(instr)] = ret;
}

void CPU::PGXP::CPU_SLTI(u32 instr, u32 rsVal)
{
  LOG_VALUES_C1(rs(instr), rsVal);

  // Rt = Rs < Imm (signed)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];

  psx_value tempImm;
  tempImm.w.h = imm(instr);
  ret.y = 0.f;
  ret.x = (g_state.pgxp_gpr[rs(instr)].x < tempImm.sw.h) ? 1.f : 0.f;
  ret.SetValid(COMP_Y);
  ret.value = BoolToUInt32(static_cast<s32>(rsVal) < imm_sext(instr));

  g_state.pgxp_gpr[rt(instr)] = ret;
}

void CPU::PGXP::CPU_SLTIU(u32 instr, u32 rsVal)
{
  LOG_VALUES_C1(rs(instr), rsVal);

  // Rt = Rs < Imm (Unsigned)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];

  psx_value tempImm;
  tempImm.w.h = imm(instr);
  ret.y = 0.f;
  ret.x = (f16Unsign(g_state.pgxp_gpr[rs(instr)].x) < tempImm.w.h) ? 1.f : 0.f;
  ret.SetValid(COMP_Y);
  ret.value = BoolToUInt32(rsVal < imm(instr));

  g_state.pgxp_gpr[rt(instr)] = ret;
}

////////////////////////////////////
// Load Upper
////////////////////////////////////
void CPU::PGXP::CPU_LUI(u32 instr)
{
  LOG_VALUES_NV();

  // Rt = Imm << 16
  g_state.pgxp_gpr[rt(instr)] = PGXP_value_zero;
  g_state.pgxp_gpr[rt(instr)].y = (float)(s16)imm(instr);
  g_state.pgxp_gpr[rt(instr)].value = static_cast<u32>(imm(instr)) << 16;
  g_state.pgxp_gpr[rt(instr)].flags = VALID_XY;
}

////////////////////////////////////
// Register Arithmetic
////////////////////////////////////

void CPU::PGXP::CPU_ADD(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs + Rt (signed)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  PGXP_value ret;
  if (rtVal == 0)
  {
    ret = g_state.pgxp_gpr[rs(instr)];
  }
  else if (rsVal == 0)
  {
    ret = g_state.pgxp_gpr[rt(instr)];
  }
  else
  {
    // iCB: Only require one valid input
    if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
        ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
    {
      MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
      MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
    }

    ret = g_state.pgxp_gpr[rs(instr)];

    ret.x = (float)f16Unsign(ret.x);
    ret.x += (float)f16Unsign(g_state.pgxp_gpr[rt(instr)].x);

    // carry on over/underflow
    float of = (ret.x > USHRT_MAX) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
    ret.x = (float)f16Sign(ret.x);
    // ret.x -= of * (USHRT_MAX + 1);
    ret.y += g_state.pgxp_gpr[rt(instr)].y + of;

    // truncate on overflow/underflow
    ret.y += (ret.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

    // TODO: decide which "z/w" component to use

    ret.flags &= (g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) | ~VALID_XY;
  }

  if (!(ret.flags & VALID_Z) && (g_state.pgxp_gpr[rt(instr)].flags & VALID_Z))
  {
    ret.z = g_state.pgxp_gpr[rt(instr)].z;
    ret.SetValid(COMP_Z);
  }

  ret.value = rsVal + rtVal;

  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_SUB(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs - Rt (signed)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];

  ret.x = (float)f16Unsign(ret.x);
  ret.x -= (float)f16Unsign(g_state.pgxp_gpr[rt(instr)].x);

  // carry on over/underflow
  float of = (ret.x > USHRT_MAX) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
  ret.x = (float)f16Sign(ret.x);
  // ret.x -= of * (USHRT_MAX + 1);
  ret.y -= g_state.pgxp_gpr[rt(instr)].y - of;

  // truncate on overflow/underflow
  ret.y += (ret.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

  ret.flags &= (g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) | ~VALID_XY;

  ret.value = rsVal - rtVal;

  if (!(ret.flags & VALID_Z) && (g_state.pgxp_gpr[rt(instr)].flags & VALID_Z))
  {
    ret.z = g_state.pgxp_gpr[rt(instr)].z;
    ret.SetValid(COMP_Z);
  }

  g_state.pgxp_gpr[rd(instr)] = ret;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_BITWISE(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs & Rt

  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  psx_value vald, vals, valt;
  vald.d = rdVal;
  vals.d = rsVal;
  valt.d = rtVal;

  PGXP_value ret;
  ret.flags = VALID_XY;

  if (vald.w.l == 0)
  {
    ret.x = 0.f;
  }
  else if (vald.w.l == vals.w.l)
  {
    ret.x = g_state.pgxp_gpr[rs(instr)].x;
    ret.SetValid(COMP_X, g_state.pgxp_gpr[rs(instr)].HasValid(COMP_X));
  }
  else if (vald.w.l == valt.w.l)
  {
    ret.x = g_state.pgxp_gpr[rt(instr)].x;
    ret.SetValid(COMP_X, g_state.pgxp_gpr[rt(instr)].HasValid(COMP_X));
  }
  else
  {
    ret.x = (float)vald.sw.l;
    ret.SetValid(COMP_X);
  }

  if (vald.w.h == 0)
  {
    ret.y = 0.f;
  }
  else if (vald.w.h == vals.w.h)
  {
    ret.y = g_state.pgxp_gpr[rs(instr)].y;
    ret.SetValid(COMP_Y, g_state.pgxp_gpr[rs(instr)].HasValid(COMP_Y));
  }
  else if (vald.w.h == valt.w.h)
  {
    ret.y = g_state.pgxp_gpr[rt(instr)].y;
    ret.SetValid(COMP_Y, g_state.pgxp_gpr[rt(instr)].HasValid(COMP_Y));
  }
  else
  {
    ret.y = (float)vald.sw.h;
    ret.SetValid(COMP_Y);
  }

  // iCB Hack: Force validity if even one half is valid
  // if ((ret.hFlags & VALID_HALF) || (ret.lFlags & VALID_HALF))
  //	ret.valid = 1;
  // /iCB Hack

  // Get a valid W
  if (g_state.pgxp_gpr[rs(instr)].HasValid(COMP_Z))
  {
    ret.z = g_state.pgxp_gpr[rs(instr)].z;
    ret.SetValid(COMP_Z);
  }
  else if (g_state.pgxp_gpr[rt(instr)].HasValid(COMP_Z))
  {
    ret.z = g_state.pgxp_gpr[rt(instr)].z;
    ret.SetValid(COMP_Z);
  }
  else
  {
    ret.z = 0.0f;
    ret.SetValid(COMP_Z, false);
  }

  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_AND_(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs & Rt
  const u32 rdVal = rsVal & rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_OR_(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs | Rt
  const u32 rdVal = rsVal | rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_XOR_(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs ^ Rt
  const u32 rdVal = rsVal ^ rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_NOR(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs NOR Rt
  const u32 rdVal = ~(rsVal | rtVal);
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_SLT(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs < Rt (signed)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];
  ret.y = 0.f;
  ret.SetValid(COMP_Y);

  ret.x = (g_state.pgxp_gpr[rs(instr)].y < g_state.pgxp_gpr[rt(instr)].y)                       ? 1.f :
          (f16Unsign(g_state.pgxp_gpr[rs(instr)].x) < f16Unsign(g_state.pgxp_gpr[rt(instr)].x)) ? 1.f :
                                                                                                  0.f;

  ret.value = BoolToUInt32(static_cast<s32>(rsVal) < static_cast<s32>(rtVal));
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_SLTU(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Rd = Rs < Rt (unsigned)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  PGXP_value ret = g_state.pgxp_gpr[rs(instr)];
  ret.y = 0.f;
  ret.SetValid(COMP_Y);

  ret.x = (f16Unsign(g_state.pgxp_gpr[rs(instr)].y) < f16Unsign(g_state.pgxp_gpr[rt(instr)].y)) ? 1.f :
          (f16Unsign(g_state.pgxp_gpr[rs(instr)].x) < f16Unsign(g_state.pgxp_gpr[rt(instr)].x)) ? 1.f :
                                                                                                  0.f;

  ret.value = BoolToUInt32(rsVal < rtVal);
  g_state.pgxp_gpr[rd(instr)] = ret;
}

////////////////////////////////////
// Register mult/div
////////////////////////////////////

void CPU::PGXP::CPU_MULT(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Hi/Lo = Rs * Rt (signed)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)] = g_state.pgxp_gpr[rs(instr)];
  CopyZIfMissing(g_state.pgxp_gpr[static_cast<u8>(Reg::lo)], g_state.pgxp_gpr[rs(instr)]);

  // Z/valid is the same
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)] = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  // Multiply out components
  xx = f16Unsign(g_state.pgxp_gpr[rs(instr)].x) * f16Unsign(g_state.pgxp_gpr[rt(instr)].x);
  xy = f16Unsign(g_state.pgxp_gpr[rs(instr)].x) * (g_state.pgxp_gpr[rt(instr)].y);
  yx = (g_state.pgxp_gpr[rs(instr)].y) * f16Unsign(g_state.pgxp_gpr[rt(instr)].x);
  yy = (g_state.pgxp_gpr[rs(instr)].y) * (g_state.pgxp_gpr[rt(instr)].y);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].x = (float)f16Sign(lx);
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].y = (float)f16Sign(ly);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].x = (float)f16Sign(hx);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].y = (float)f16Sign(hy);

  // compute PSX value
  const u64 result = static_cast<u64>(static_cast<s64>(SignExtend64(rsVal)) * static_cast<s64>(SignExtend64(rtVal)));
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value = Truncate32(result >> 32);
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value = Truncate32(result);
}

void CPU::PGXP::CPU_MULTU(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Hi/Lo = Rs * Rt (unsigned)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)] = g_state.pgxp_gpr[rs(instr)];
  CopyZIfMissing(g_state.pgxp_gpr[static_cast<u8>(Reg::lo)], g_state.pgxp_gpr[rs(instr)]);

  // Z/valid is the same
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)] = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  // Multiply out components
  xx = f16Unsign(g_state.pgxp_gpr[rs(instr)].x) * f16Unsign(g_state.pgxp_gpr[rt(instr)].x);
  xy = f16Unsign(g_state.pgxp_gpr[rs(instr)].x) * f16Unsign(g_state.pgxp_gpr[rt(instr)].y);
  yx = f16Unsign(g_state.pgxp_gpr[rs(instr)].y) * f16Unsign(g_state.pgxp_gpr[rt(instr)].x);
  yy = f16Unsign(g_state.pgxp_gpr[rs(instr)].y) * f16Unsign(g_state.pgxp_gpr[rt(instr)].y);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].x = (float)f16Sign(lx);
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].y = (float)f16Sign(ly);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].x = (float)f16Sign(hx);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].y = (float)f16Sign(hy);

  // compute PSX value
  const u64 result = ZeroExtend64(rsVal) * ZeroExtend64(rtVal);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value = Truncate32(result >> 32);
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value = Truncate32(result);
}

void CPU::PGXP::CPU_DIV(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Lo = Rs / Rt (signed)
  // Hi = Rs % Rt (signed)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  //// iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)] = g_state.pgxp_gpr[rs(instr)];
  CopyZIfMissing(g_state.pgxp_gpr[static_cast<u8>(Reg::lo)], g_state.pgxp_gpr[rs(instr)]);

  // Z/valid is the same
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)] = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];

  double vs = f16Unsign(g_state.pgxp_gpr[rs(instr)].x) + (g_state.pgxp_gpr[rs(instr)].y) * (double)(1 << 16);
  double vt = f16Unsign(g_state.pgxp_gpr[rt(instr)].x) + (g_state.pgxp_gpr[rt(instr)].y) * (double)(1 << 16);

  double lo = vs / vt;
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].y = (float)f16Sign(f16Overflow(lo));
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].x = (float)f16Sign(lo);

  double hi = fmod(vs, vt);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].y = (float)f16Sign(f16Overflow(hi));
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].x = (float)f16Sign(hi);

  // compute PSX value
  if (static_cast<s32>(rtVal) == 0)
  {
    // divide by zero
    g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value =
      (static_cast<s32>(rsVal) >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
    g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value = static_cast<u32>(static_cast<s32>(rsVal));
  }
  else if (rsVal == UINT32_C(0x80000000) && static_cast<s32>(rtVal) == -1)
  {
    // unrepresentable
    g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value = UINT32_C(0x80000000);
    g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value = 0;
  }
  else
  {
    g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value =
      static_cast<u32>(static_cast<s32>(rsVal) / static_cast<s32>(rtVal));
    g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value =
      static_cast<u32>(static_cast<s32>(rsVal) % static_cast<s32>(rtVal));
  }
}

void CPU::PGXP::CPU_DIVU(u32 instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(rs(instr), rsVal, rt(instr), rtVal);

  // Lo = Rs / Rt (unsigned)
  // Hi = Rs % Rt (unsigned)
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  //// iCB: Only require one valid input
  if (((g_state.pgxp_gpr[rt(instr)].flags & VALID_XY) != VALID_XY) !=
      ((g_state.pgxp_gpr[rs(instr)].flags & VALID_XY) != VALID_XY))
  {
    MakeValid(&g_state.pgxp_gpr[rs(instr)], rsVal);
    MakeValid(&g_state.pgxp_gpr[rt(instr)], rtVal);
  }

  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)] = g_state.pgxp_gpr[rs(instr)];
  CopyZIfMissing(g_state.pgxp_gpr[static_cast<u8>(Reg::lo)], g_state.pgxp_gpr[rs(instr)]);

  // Z/valid is the same
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)] = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];

  double vs = f16Unsign(g_state.pgxp_gpr[rs(instr)].x) + f16Unsign(g_state.pgxp_gpr[rs(instr)].y) * (double)(1 << 16);
  double vt = f16Unsign(g_state.pgxp_gpr[rt(instr)].x) + f16Unsign(g_state.pgxp_gpr[rt(instr)].y) * (double)(1 << 16);

  double lo = vs / vt;
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].y = (float)f16Sign(f16Overflow(lo));
  g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].x = (float)f16Sign(lo);

  double hi = fmod(vs, vt);
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].y = (float)f16Sign(f16Overflow(hi));
  g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].x = (float)f16Sign(hi);

  if (rtVal == 0)
  {
    // divide by zero
    g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value = UINT32_C(0xFFFFFFFF);
    g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value = rsVal;
  }
  else
  {
    g_state.pgxp_gpr[static_cast<u8>(Reg::lo)].value = rsVal / rtVal;
    g_state.pgxp_gpr[static_cast<u8>(Reg::hi)].value = rsVal % rtVal;
  }
}

////////////////////////////////////
// Shift operations (sa)
////////////////////////////////////
void CPU::PGXP::CPU_SLL(u32 instr, u32 rtVal)
{
  LOG_VALUES_C1(rt(instr), rtVal);

  // Rd = Rt << Sa
  const u32 rdVal = rtVal << sa(instr);
  const u32 sh = sa(instr);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  PGXP_value ret = g_state.pgxp_gpr[rt(instr)];

  // TODO: Shift flags
  double x = f16Unsign(g_state.pgxp_gpr[rt(instr)].x);
  double y = f16Unsign(g_state.pgxp_gpr[rt(instr)].y);
  if (sh >= 32)
  {
    x = 0.f;
    y = 0.f;
  }
  else if (sh == 16)
  {
    y = f16Sign(x);
    x = 0.f;
  }
  else if (sh >= 16)
  {
    y = x * (1 << (sh - 16));
    y = f16Sign(y);
    x = 0.f;
  }
  else
  {
    x = x * (1 << sh);
    y = y * (1 << sh);
    y += f16Overflow(x);
    x = f16Sign(x);
    y = f16Sign(y);
  }

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_SRL(u32 instr, u32 rtVal)
{
  LOG_VALUES_C1(rt(instr), rtVal);

  // Rd = Rt >> Sa
  const u32 rdVal = rtVal >> sa(instr);
  const u32 sh = sa(instr);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);

  double x = g_state.pgxp_gpr[rt(instr)].x, y = f16Unsign(g_state.pgxp_gpr[rt(instr)].y);

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.d = iY.d >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (g_state.pgxp_gpr[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  PGXP_value ret = g_state.pgxp_gpr[rt(instr)];
  ret.x = (float)x;
  ret.y = (float)y;
  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_SRA(u32 instr, u32 rtVal)
{
  LOG_VALUES_C1(rt(instr), rtVal);

  // Rd = Rt >> Sa
  const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> sa(instr));
  const u32 sh = sa(instr);
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);
  PGXP_value ret = g_state.pgxp_gpr[rt(instr)];

  double x = g_state.pgxp_gpr[rt(instr)].x, y = g_state.pgxp_gpr[rt(instr)].y;

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.sd = iY.sd >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (g_state.pgxp_gpr[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  ret.x = (float)x;
  ret.y = (float)y;

  // Use low precision/rounded values when we're not shifting an entire component,
  // and it's not originally from a 3D value. Too many false positives in P2/etc.
  // What we probably should do is not set the valid flag on non-3D values to begin
  // with, only letting them become valid when used in another expression.
  if (!(ret.flags & VALID_Z) && sh < 16)
  {
    ret.flags = 0;
    MakeValid(&ret, rdVal);
  }

  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

////////////////////////////////////
// Shift operations variable
////////////////////////////////////
void CPU::PGXP::CPU_SLLV(u32 instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(rt(instr), rtVal, rs(instr), rsVal);

  // Rd = Rt << Rs
  const u32 rdVal = rtVal << rsVal;
  const u32 sh = rsVal & 0x1F;
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);

  double x = f16Unsign(g_state.pgxp_gpr[rt(instr)].x);
  double y = f16Unsign(g_state.pgxp_gpr[rt(instr)].y);
  if (sh >= 32)
  {
    x = 0.f;
    y = 0.f;
  }
  else if (sh == 16)
  {
    y = f16Sign(x);
    x = 0.f;
  }
  else if (sh >= 16)
  {
    y = x * (1 << (sh - 16));
    y = f16Sign(y);
    x = 0.f;
  }
  else
  {
    x = x * (1 << sh);
    y = y * (1 << sh);
    y += f16Overflow(x);
    x = f16Sign(x);
    y = f16Sign(y);
  }

  PGXP_value ret = g_state.pgxp_gpr[rt(instr)];
  ret.x = (float)x;
  ret.y = (float)y;
  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_SRLV(u32 instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(rt(instr), rtVal, rs(instr), rsVal);

  // Rd = Rt >> Sa
  const u32 rdVal = rtVal >> rsVal;
  const u32 sh = rsVal & 0x1F;
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);

  double x = g_state.pgxp_gpr[rt(instr)].x, y = f16Unsign(g_state.pgxp_gpr[rt(instr)].y);

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.d = iY.d >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (g_state.pgxp_gpr[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  PGXP_value ret = g_state.pgxp_gpr[rt(instr)];
  ret.x = (float)x;
  ret.y = (float)y;
  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_SRAV(u32 instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(rt(instr), rtVal, rs(instr), rsVal);

  // Rd = Rt >> Sa
  const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> rsVal);
  const u32 sh = rsVal & 0x1F;
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);
  Validate(&g_state.pgxp_gpr[rs(instr)], rsVal);

  double x = g_state.pgxp_gpr[rt(instr)].x, y = g_state.pgxp_gpr[rt(instr)].y;

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.sd = iY.sd >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (g_state.pgxp_gpr[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  PGXP_value ret = g_state.pgxp_gpr[rt(instr)];
  x = f16Sign(x);
  y = f16Sign(y);
  ret.x = (float)x;
  ret.y = (float)y;
  ret.value = rdVal;
  g_state.pgxp_gpr[rd(instr)] = ret;
}

void CPU::PGXP::CPU_MFC0(u32 instr, u32 rdVal)
{
  LOG_VALUES_1(TinyString::from_format("cop0_{}", rd(instr)).c_str(), rdVal, &g_state.pgxp_cop0[rd(instr)]);

  // CPU[Rt] = CP0[Rd]
  Validate(&g_state.pgxp_cop0[rd(instr)], rdVal);
  g_state.pgxp_gpr[rt(instr)] = g_state.pgxp_cop0[rd(instr)];
  g_state.pgxp_gpr[rt(instr)].value = rdVal;
}

void CPU::PGXP::CPU_MTC0(u32 instr, u32 rdVal, u32 rtVal)
{
  LOG_VALUES_C1(rt(instr), rtVal);

  // CP0[Rd] = CPU[Rt]
  Validate(&g_state.pgxp_gpr[rt(instr)], rtVal);
  g_state.pgxp_cop0[rd(instr)] = g_state.pgxp_gpr[rt(instr)];
  g_state.pgxp_cop0[rd(instr)].value = rdVal;
}
