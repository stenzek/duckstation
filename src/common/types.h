// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

// Force inline helper
#ifndef ALWAYS_INLINE
#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif
#endif

// Force inline in non-debug helper
#ifdef _DEBUG
#define ALWAYS_INLINE_RELEASE inline
#else
#define ALWAYS_INLINE_RELEASE ALWAYS_INLINE
#endif

// unreferenced parameter macro
#ifndef UNREFERENCED_VARIABLE
#if defined(__GNUC__) || defined(__clang__) || defined(__EMSCRIPTEN__)
#define UNREFERENCED_VARIABLE(P) (void)(P)
#else
#define UNREFERENCED_VARIABLE(P) (P)
#endif
#endif

// countof macro
#ifndef countof
#ifdef _countof
#define countof _countof
#else
template<typename T, size_t N>
char (&__countof_ArraySizeHelper(T (&array)[N]))[N];
#define countof(array) (sizeof(__countof_ArraySizeHelper(array)))
#endif
#endif

// offsetof macro
#ifndef offsetof
#define offsetof(st, m) ((size_t)((char*)&((st*)(0))->m - (char*)0))
#endif

#ifdef __GNUC__
#define printflike(n, m) __attribute__((format(printf, n, m)))
#else
#define printflike(n, m)
#endif

// [[noreturn]] which can be used on function pointers.
#ifdef _MSC_VER
// __declspec(noreturn) produces error C3829.
#define NORETURN_FUNCTION_POINTER
#else
#define NORETURN_FUNCTION_POINTER __attribute__((noreturn))
#endif

// __assume, potentially enables optimization.
#ifdef _MSC_VER
#define ASSUME(x) __assume(x)
#else
#define ASSUME(x)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(x))                                                                                                          \
      __builtin_unreachable();                                                                                         \
  } while (0)
#endif

// disable warnings that show up at warning level 4
// TODO: Move to build system instead
#ifdef _MSC_VER
#pragma warning(disable : 4201) // warning C4201: nonstandard extension used : nameless struct/union
#pragma warning(disable : 4100) // warning C4100: 'Platform' : unreferenced formal parameter
#pragma warning(disable : 4355) // warning C4355: 'this' : used in base member initializer list
#endif

using s8 = int8_t;
using u8 = uint8_t;
using s16 = int16_t;
using u16 = uint16_t;
using s32 = int32_t;
using u32 = uint32_t;
using s64 = int64_t;
using u64 = uint64_t;

// Enable use of static_assert in constexpr if
template<class T>
struct dependent_false : std::false_type
{
};
template<int T>
struct dependent_int_false : std::false_type
{
};

// Architecture detection.
#if defined(_MSC_VER)

#if defined(_M_X64)
#define CPU_ARCH_X64 1
#elif defined(_M_IX86)
#define CPU_ARCH_X86 1
#elif defined(_M_ARM64)
#define CPU_ARCH_ARM64 1
#elif defined(_M_ARM)
#define CPU_ARCH_ARM32 1
#else
#error Unknown architecture.
#endif

#elif defined(__GNUC__) || defined(__clang__)

#if defined(__x86_64__)
#define CPU_ARCH_X64 1
#elif defined(__i386__)
#define CPU_ARCH_X86 1
#elif defined(__aarch64__)
#define CPU_ARCH_ARM64 1
#elif defined(__arm__)
#define CPU_ARCH_ARM32 1
#elif defined(__riscv) && __riscv_xlen == 64
#define CPU_ARCH_RISCV64 1
#else
#error Unknown architecture.
#endif

#else

#error Unknown compiler.

#endif

#if defined(CPU_ARCH_X64)
#define CPU_ARCH_STR "x64"
#elif defined(CPU_ARCH_X86)
#define CPU_ARCH_STR "x86"
#elif defined(CPU_ARCH_ARM32)
#define CPU_ARCH_STR "arm32"
#elif defined(CPU_ARCH_ARM64)
#define CPU_ARCH_STR "arm64"
#elif defined(CPU_ARCH_RISCV64)
#define CPU_ARCH_STR "riscv64"
#else
#define CPU_ARCH_STR "Unknown"
#endif

// OS detection.
#if defined(_WIN32)
#define TARGET_OS_STR "Windows"
#elif defined(__ANDROID__)
#define TARGET_OS_STR "Android"
#elif defined(__linux__)
#define TARGET_OS_STR "Linux"
#elif defined(__FreeBSD__)
#define TARGET_OS_STR "FreeBSD"
#elif defined(__APPLE__)
#define TARGET_OS_STR "macOS"
#else
#define TARGET_OS_STR "Unknown"
#endif

// Host page sizes.
#if defined(OVERRIDE_HOST_PAGE_SIZE)
static constexpr u32 HOST_PAGE_SIZE = OVERRIDE_HOST_PAGE_SIZE;
static constexpr u32 HOST_PAGE_MASK = HOST_PAGE_SIZE - 1;
static constexpr u32 HOST_PAGE_SHIFT = std::bit_width(HOST_PAGE_MASK);
#elif defined(__APPLE__) && defined(__aarch64__)
static constexpr u32 HOST_PAGE_SIZE = 0x4000;
static constexpr u32 HOST_PAGE_MASK = HOST_PAGE_SIZE - 1;
static constexpr u32 HOST_PAGE_SHIFT = 14;
#else
static constexpr u32 HOST_PAGE_SIZE = 0x1000;
static constexpr u32 HOST_PAGE_MASK = HOST_PAGE_SIZE - 1;
static constexpr u32 HOST_PAGE_SHIFT = 12;
#endif

// Host cache line sizes.
#if defined(__APPLE__) && defined(__aarch64__)
static constexpr u32 HOST_CACHE_LINE_SIZE = 128; // Apple Silicon uses 128b cache lines.
#else
static constexpr u32 HOST_CACHE_LINE_SIZE = 64; // Everything else is 64b.
#endif

// Enum class bitwise operators
#define IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(type_)                                                                  \
  ALWAYS_INLINE constexpr type_ operator&(type_ lhs, type_ rhs)                                                        \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) &                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator|(type_ lhs, type_ rhs)                                                        \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) |                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator^(type_ lhs, type_ rhs)                                                        \
  {                                                                                                                    \
    return static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) ^                                    \
                              static_cast<std::underlying_type<type_>::type>(rhs));                                    \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_ operator~(type_ val)                                                                   \
  {                                                                                                                    \
    return static_cast<type_>(~static_cast<std::underlying_type<type_>::type>(val));                                   \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator&=(type_& lhs, type_ rhs)                                                     \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) &                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator|=(type_& lhs, type_ rhs)                                                     \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) |                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }                                                                                                                    \
  ALWAYS_INLINE constexpr type_& operator^=(type_& lhs, type_ rhs)                                                     \
  {                                                                                                                    \
    lhs = static_cast<type_>(static_cast<std::underlying_type<type_>::type>(lhs) ^                                     \
                             static_cast<std::underlying_type<type_>::type>(rhs));                                     \
    return lhs;                                                                                                        \
  }

// Compute the address of a base type given a field offset.
#define BASE_FROM_RECORD_FIELD(ptr, base_type, field) ((base_type*)(((char*)ptr) - offsetof(base_type, field)))
