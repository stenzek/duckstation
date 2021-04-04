#pragma once

#if defined(_MSC_VER)

#if defined(_M_X64)
#define CPU_X64 1
#elif defined(_M_IX86)
#define CPU_X86 1
#elif defined(_M_ARM64)
#define CPU_AARCH64 1
#elif defined(_M_ARM)
#define CPU_AARCH32 1
#else
#error Unknown architecture.
#endif

#elif defined(__GNUC__) || defined(__clang__)

#if defined(__x86_64__)
#define CPU_X64 1
#elif defined(__i386__)
#define CPU_X86 1
#elif defined(__aarch64__)
#define CPU_AARCH64 1
#elif defined(__arm__)
#define CPU_AARCH32 1
#else
#error Unknown architecture.
#endif

#else

#error Unknown compiler.

#endif

#if defined(CPU_X64)
#define CPU_ARCH_STR "x64"
#elif defined(CPU_X86)
#define CPU_ARCH_STR "x86"
#elif defined(CPU_AARCH32)
#define CPU_ARCH_STR "AArch32"
#elif defined(CPU_AARCH64)
#define CPU_ARCH_STR "AArch64"
#else
#define CPU_ARCH_STR "Unknown"
#endif

#if defined(_WIN32)
#define SYSTEM_STR "Windows"
#elif defined(__ANDROID__)
#define SYSTEM_STR "Android"
#elif defined(__linux__)
#define SYSTEM_STR "Linux"
#elif defined(__FreeBSD__)
#define SYSTEM_STR "FreeBSD"
#elif defined(__APPLE__)
#define SYSTEM_STR "macOS"
#else
#define SYSTEM_STR "Unknown"
#endif
