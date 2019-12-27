#pragma once
#include "cpu_types.h"

#if defined(Y_CPU_X64)
#define XBYAK_NO_OP_NAMES 1
#include "xbyak.h"
#elif defined(Y_CPU_AARCH64)
#include <vixl/aarch64/constants-aarch64.h>
#include <vixl/aarch64/macro-assembler-aarch64.h>
#endif

namespace CPU {

class Core;
class CodeCache;

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

#if defined(Y_CPU_X64)

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

#elif defined(Y_CPU_AARCH64)

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

} // namespace Recompiler

} // namespace CPU
