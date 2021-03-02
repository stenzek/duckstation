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
  BilinearBinAlpha,
  JINC2,
  JINC2BinAlpha,
  xBR,
  xBRBinAlpha,
  Count
};

enum GPUDownsampleMode : u8
{
  Disabled,
  Box,
  Adaptive,
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
  Auto,
  R4_3,
  R16_9,
  R16_10,
  R19_9,
  R20_9,
  R21_9,
  R32_9,
  R8_7,
  R5_4,
  R3_2,
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
  AnalogJoystick,
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

enum class MultitapMode
{
  Disabled,
  Port1Only,
  Port2Only,
  BothPorts,
  Count
};

enum : u32
{
  NUM_CONTROLLER_AND_CARD_PORTS = 8,
  NUM_MULTITAPS = 2
};

enum class CPUFastmemMode
{
  Disabled,
  MMap,
  LUT,
  Count
};

enum : size_t
{
  HOST_PAGE_SIZE = 4096,
  HOST_PAGE_OFFSET_MASK = HOST_PAGE_SIZE - 1,
};
