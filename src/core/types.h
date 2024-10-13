// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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
using GlobalTicks = u64;
using GameHash = u64;

enum class ConsoleRegion : u8
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
  NonPS1,
  Count
};

enum class CPUExecutionMode : u8
{
  Interpreter,
  CachedInterpreter,
  Recompiler,
  NewRec,
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
  Automatic,
#ifdef _WIN32
  HardwareD3D11,
  HardwareD3D12,
#endif
#ifdef __APPLE__
  HardwareMetal,
#endif
#ifdef ENABLE_VULKAN
  HardwareVulkan,
#endif
#ifdef ENABLE_OPENGL
  HardwareOpenGL,
#endif
  Software,
  Count
};

enum class DisplayDeinterlacingMode : u8
{
  Disabled,
  Weave,
  Blend,
  Adaptive,
  Progressive,
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

enum class GPUDownsampleMode : u8
{
  Disabled,
  Box,
  Adaptive,
  Count
};

enum class GPUWireframeMode : u8
{
  Disabled,
  OverlayWireframe,
  OnlyWireframe,
  Count,
};

enum class GPULineDetectMode : u8
{
  Disabled,
  Quads,
  BasicTriangles,
  AggressiveTriangles,
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
  MatchWindow,
  Custom,
  R4_3,
  R16_9,
  R19_9,
  R20_9,
  PAR1_1,
  Count
};

enum class DisplayAlignment : u8
{
  LeftOrTop,
  Center,
  RightOrBottom,
  Count
};

enum class DisplayRotation : u8
{
  Normal,
  Rotate90,
  Rotate180,
  Rotate270,
  Count
};

enum class DisplayScalingMode : u8
{
  Nearest,
  NearestInteger,
  BilinearSmooth,
  BilinearSharp,
  BilinearInteger,
  Count
};

enum class DisplayExclusiveFullscreenControl : u8
{
  Automatic,
  Disallowed,
  Allowed,
  Count
};

enum class DisplayScreenshotMode : u8
{
  ScreenResolution,
  InternalResolution,
  UncorrectedInternalResolution,
  Count
};

enum class DisplayScreenshotFormat : u8
{
  PNG,
  JPEG,
  WebP,
  Count
};

enum class ControllerType : u8
{
  None,
  DigitalController,
  AnalogController,
  AnalogJoystick,
  GunCon,
  PlayStationMouse,
  NeGcon,
  NeGconRumble,
  Justifier,
  Count
};

enum class MemoryCardType
{
  None,
  Shared,
  PerGame,
  PerGameTitle,
  PerGameFileTitle,
  NonPersistent,
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
  NUM_MULTITAPS = 2,
  NUM_CONTROLLER_AND_CARD_PORTS_PER_MULTITAP = NUM_CONTROLLER_AND_CARD_PORTS / NUM_MULTITAPS,
};

enum class CPUFastmemMode : u8
{
  Disabled,
  MMap,
  LUT,
  Count
};

enum class CDROMMechaconVersion : u8
{
  VC0A,
  VC0B,
  VC1A,
  VC1B,
  VD1,
  VC2,
  VC1,
  VC2J,
  VC2A,
  VC2B,
  VC3A,
  VC3B,
  VC3C,

  Count,
};

enum class SaveStateCompressionMode : u8
{
  Uncompressed,
  DeflateLow,
  DeflateDefault,
  DeflateHigh,
  ZstLow,
  ZstDefault,
  ZstHigh,

  Count,
};

enum class ForceVideoTimingMode : u8
{
  Disabled,
  NTSC,
  PAL,
  
  Count,
};
