#pragma once
#include "common/cpu_detect.h"
#include "cpu_types.h"

#if defined(CPU_X64)

// We need to include windows.h before xbyak does..
#ifdef WIN32
#include "common/windows_headers.h"
#endif

#define XBYAK_NO_OP_NAMES 1
#include "xbyak.h"

#elif defined(CPU_AARCH64)

#include <vixl/aarch64/constants-aarch64.h>
#include <vixl/aarch64/macro-assembler-aarch64.h>

#endif

namespace CPU {

namespace Recompiler {

class CodeGenerator;
class RegisterCache;

enum RegSize : u8
{
  RegSize_8,
  RegSize_16,
  RegSize_32,
  RegSize_64,
};

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

#if defined(CPU_X64)

using HostReg = Xbyak::Operand::Code;
using CodeEmitter = Xbyak::CodeGenerator;
using LabelType = Xbyak::Label;
enum : u32
{
  HostReg_Count = 16
};
constexpr HostReg HostReg_Invalid = static_cast<HostReg>(HostReg_Count);
constexpr RegSize HostPointerSize = RegSize_64;

// A reasonable "maximum" number of bytes per instruction.
constexpr u32 MAX_NEAR_HOST_BYTES_PER_INSTRUCTION = 64;
constexpr u32 MAX_FAR_HOST_BYTES_PER_INSTRUCTION = 128;

// Are shifts implicitly masked to 0..31?
constexpr bool SHIFTS_ARE_IMPLICITLY_MASKED = true;

// Alignment of code stoarge.
constexpr u32 CODE_STORAGE_ALIGNMENT = 4096;

// ABI selection
#if defined(WIN32)
#define ABI_WIN64 1
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__)
#define ABI_SYSV 1
#else
#error Unknown ABI.
#endif

#elif defined(CPU_AARCH64)

using HostReg = unsigned;
using CodeEmitter = vixl::aarch64::MacroAssembler;
using LabelType = vixl::aarch64::Label;
enum : u32
{
  HostReg_Count = vixl::aarch64::kNumberOfRegisters
};
constexpr HostReg HostReg_Invalid = static_cast<HostReg>(HostReg_Count);
constexpr RegSize HostPointerSize = RegSize_64;

// A reasonable "maximum" number of bytes per instruction.
constexpr u32 MAX_NEAR_HOST_BYTES_PER_INSTRUCTION = 64;
constexpr u32 MAX_FAR_HOST_BYTES_PER_INSTRUCTION = 128;

// Are shifts implicitly masked to 0..31?
constexpr bool SHIFTS_ARE_IMPLICITLY_MASKED = true;

// Alignment of code stoarge.
constexpr u32 CODE_STORAGE_ALIGNMENT = 4096;

#else

using HostReg = int;

class CodeEmitter
{
};

enum : u32
{
  HostReg_Count = 1
};

constexpr HostReg HostReg_Invalid = static_cast<HostReg>(HostReg_Count);
constexpr RegSize HostPointerSize = RegSize_64;
constexpr bool SHIFTS_ARE_IMPLICITLY_MASKED = false;

#endif

struct LoadStoreBackpatchInfo
{
  void* host_pc;            // pointer to instruction which will fault
  void* host_slowmem_pc;    // pointer to slowmem callback code
  u32 host_code_size;       // size of the fastmem load as well as the add for cycles
  HostReg address_host_reg; // register containing the guest address to load/store
  HostReg value_host_reg;   // register containing the source/destination
  PhysicalMemoryAddress guest_pc;
};

} // namespace Recompiler

} // namespace CPU
