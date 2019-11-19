#pragma once
#include "cpu_types.h"

#if defined(Y_CPU_X64)
#define XBYAK_NO_OP_NAMES 1
#include "xbyak.h"
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

#if defined(Y_CPU_X64)
using HostReg = Xbyak::Operand::Code;
using CodeEmitter = Xbyak::CodeGenerator;
enum : u32
{
  HostReg_Count = 16
};
constexpr HostReg HostReg_Invalid = static_cast<HostReg>(HostReg_Count);
constexpr RegSize HostPointerSize = RegSize_64;

// A reasonable "maximum" number of bytes per instruction.
constexpr u32 MAX_HOST_BYTES_PER_INSTRUCTION = 128;

#else
using HostReg = void;
using CodeEmitter = void;
enum : u32
{
  HostReg_Count = 0
};
constexpr HostReg HostReg_Invalid = static_cast<HostReg>(HostReg_Count);
constexpr OperandSize HostPointerSize = OperandSize_64;
#endif

} // namespace Recompiler

} // namespace CPU