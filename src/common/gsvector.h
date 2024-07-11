// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/intrin.h"

#if defined(CPU_ARCH_SSE)
#include "common/gsvector_sse.h"
#elif defined(CPU_ARCH_NEON)
#include "common/gsvector_neon.h"
#else
#include "common/gsvector_nosimd.h"
#endif
