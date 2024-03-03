// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

enum class RenderAPI : u32
{
  None,
  D3D11,
  D3D12,
  Vulkan,
  OpenGL,
  OpenGLES,
  Metal
};

enum class DisplaySyncMode : u8
{
  Disabled,
  VSync,
  VSyncRelaxed,
  VRR,
  Count
};
