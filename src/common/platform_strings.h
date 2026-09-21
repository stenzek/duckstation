// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#ifdef CPU_ARCH_X64
#define CPU_ARCH_STR "x64"
#elifdef CPU_ARCH_ARM32
#define CPU_ARCH_STR "arm32"
#elifdef CPU_ARCH_ARM64
#define CPU_ARCH_STR "arm64"
#elifdef CPU_ARCH_RISCV64
#define CPU_ARCH_STR "riscv64"
#elifdef CPU_ARCH_LOONGARCH64
#define CPU_ARCH_STR "loongarch64"
#else
#error Unknown architecture.
#endif

// OS detection.
#ifdef _WIN32
#define TARGET_OS_STR "Windows"
#elifdef __linux__
#define TARGET_OS_STR "Linux"
#elifdef __APPLE__
#define TARGET_OS_STR "macOS"
#else
#error Unknown OS.
#endif
