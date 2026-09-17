// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

inline constexpr u32 SHADER_CACHE_VERSION = 41;

// Used to tag opaque keys.
enum class ShaderCacheKeyType : u16
{
	HWBatchVertex,
	HWBatchFragment,
};
