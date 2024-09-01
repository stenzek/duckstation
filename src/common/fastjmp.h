// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>

struct fastjmp_buf
{
#if defined(_WIN32) && defined(_M_AMD64)
  static constexpr std::size_t BUF_SIZE = 240;
#elif defined(_M_ARM64) || defined(__aarch64__)
  static constexpr std::size_t BUF_SIZE = 168;
#elif defined(_M_ARM) || defined(__arm__)
  static constexpr std::size_t BUF_SIZE = 112;
#elif defined(__x86_64__)
  static constexpr std::size_t BUF_SIZE = 64;
#elif defined(_M_IX86) || defined(__i386__)
  static constexpr std::size_t BUF_SIZE = 24;
#elif defined(__riscv) && __riscv_xlen == 64
  static constexpr std::size_t BUF_SIZE = 208;
#else
#error Unknown architecture.
#endif

  alignas(16) std::uint8_t buf[BUF_SIZE];
};

extern "C" {
int fastjmp_set(fastjmp_buf* buf);
[[noreturn]] void fastjmp_jmp(const fastjmp_buf* buf, int ret);
}
