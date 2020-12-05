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
