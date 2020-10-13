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

enum class ConsoleRegion
{
  Auto,
  NTSC_J,
  NTSC_U,
  PAL,
  Count
};

enum class DiscRegion : u8
{
  NTSC_J, // SCEI
  NTSC_U, // SCEA
  PAL,    // SCEE
  Other,
  Count
};

enum class CPUExecutionMode : u8
{
  Interpreter,
  CachedInterpreter,
  Recompiler,
  Count
};

enum class PGXPMode : u8
{
  Disabled,
  Memory,
  CPU
};

enum class GPURenderer : u8
{
#ifdef WIN32
  HardwareD3D11,
#endif
  HardwareVulkan,
  HardwareOpenGL,
  Software,
  Count
};

enum class GPUTextureFilter : u8
{
  Nearest,
  Bilinear,
  JINC2,
  xBR,
  Count
};

enum class DisplayCropMode : u8
{
  None,
  Overscan,
  Borders,
  Count
};

enum class DisplayAspectRatio : u8
{
  R4_3,
  R16_9,
  R16_10,
  R21_9,
  R8_7,
  R2_1,
  R1_1,
  PAR1_1,
  Count
};

enum class AudioBackend : u8
{
  Null,
  Cubeb,
#ifndef ANDROID
  SDL,
#else
  OpenSLES,
#endif
  Count
};

enum class ControllerType
{
  None,
  DigitalController,
  AnalogController,
  NamcoGunCon,
  PlayStationMouse,
  NeGcon,
  Count
};

enum class MemoryCardType
{
  None,
  Shared,
  PerGame,
  PerGameTitle,
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
