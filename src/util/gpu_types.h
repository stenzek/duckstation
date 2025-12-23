// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

enum class RenderAPI : u8
{
  None,
  D3D11,
  D3D12,
  Vulkan,
  OpenGL,
  OpenGLES,
  Metal
};

enum class GPUVSyncMode : u8
{
  Disabled,
  FIFO,
  Mailbox,
  Count
};

enum class GPUTextureFormat : u8
{
  Unknown,
  RGBA8,
  BGRA8,
  RGB565,
  RGB5A1,
  A1BGR5,
  R8,
  D16,
  D24S8,
  D32F,
  D32FS8,
  R16,
  R16I,
  R16U,
  R16F,
  R32I,
  R32U,
  R32F,
  RG8,
  RG16,
  RG16F,
  RG32F,
  RGBA16,
  RGBA16F,
  RGBA32F,
  RGB10A2,
  SRGBA8,
  BC1, ///< BC1, aka DXT1 compressed texture
  BC2, ///< BC2, aka DXT2/3 compressed texture
  BC3, ///< BC3, aka DXT4/5 compressed texture
  BC7, ///< BC7, aka BPTC compressed texture
  MaxCount,
};

enum class GPUShaderStage : u8
{
  Vertex,
  Fragment,
  Geometry,
  Compute,

  MaxCount
};

enum class GPUShaderLanguage : u8
{
  None,
  HLSL,
  GLSL,
  GLSLES,
  GLSLVK,
  MSL,
  SPV,
  Count
};

enum class GPUDriverType : u16
{
  MobileFlag = 0x100,
  SoftwareFlag = 0x200,

  Unknown = 0,
  AMDProprietary = 1,
  AMDMesa = 2,
  IntelProprietary = 3,
  IntelMesa = 4,
  NVIDIAProprietary = 5,
  NVIDIAMesa = 6,
  AppleProprietary = 7,
  AppleMesa = 8,
  DozenMesa = 9,

  ImaginationProprietary = MobileFlag | 1,
  ImaginationMesa = MobileFlag | 2,
  ARMProprietary = MobileFlag | 3,
  ARMMesa = MobileFlag | 4,
  QualcommProprietary = MobileFlag | 5,
  QualcommMesa = MobileFlag | 6,
  BroadcomProprietary = MobileFlag | 7,
  BroadcomMesa = MobileFlag | 8,

  LLVMPipe = SoftwareFlag | 1,
  SwiftShader = SoftwareFlag | 2,
  WARP = SoftwareFlag | 3,
};
