#pragma once
#include "common/types.h"

// Physical memory addresses are 32-bits wide
using PhysicalMemoryAddress = u32;
using VirtualMemoryAddress = u32;

enum class MemoryAccessType : u32
{
  Read,
  Write
};
enum class MemoryAccessSize : u32
{
  Byte,
  HalfWord,
  Word
};

using TickCount = s32;

static constexpr TickCount MASTER_CLOCK = 44100 * 0x300; // 33868800Hz or 33.8688MHz, also used as CPU clock
static constexpr TickCount MAX_SLICE_SIZE = MASTER_CLOCK / 10;

enum class ConsoleRegion
{
  Auto,
  NTSC_J,
  NTSC_U,
  PAL,
  Count
};

enum class CPUExecutionMode : u8
{
  Interpreter,
  CachedInterpreter,
  Recompiler,
  Count
};

enum class GPURenderer : u8
{
#ifdef WIN32
  HardwareD3D11,
#endif
  HardwareOpenGL,
  Software,
  Count
};

enum class AudioBackend : u8
{
  Null,
  Cubeb,
  SDL,
  Count
};

enum class ControllerType
{
  None,
  DigitalController,
  AnalogController,
  Count
};

enum : u32
{
  NUM_CONTROLLER_AND_CARD_PORTS = 2
};


enum : u32
{
  CPU_CODE_CACHE_PAGE_SIZE = 1024,
  CPU_CODE_CACHE_PAGE_COUNT = 0x200000 / CPU_CODE_CACHE_PAGE_SIZE
};
