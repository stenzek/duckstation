// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

// Includes appropriate intrinsic header based on platform.

#pragma once

#include "types.h"

#if defined(CPU_ARCH_X86) || defined(CPU_ARCH_X64)
#define CPU_ARCH_SSE 1
#include <emmintrin.h>
#elif defined(CPU_ARCH_ARM64)
#define CPU_ARCH_NEON 1
#ifdef _MSC_VER
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

#ifdef __APPLE__
#include <stdlib.h> // alloca
#else
#include <malloc.h> // alloca
#endif
