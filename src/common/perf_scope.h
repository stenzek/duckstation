// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>, PCSX2 Team
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "types.h"

class PerfScope
{
public:
  constexpr PerfScope(const char* prefix) : m_prefix(prefix) {}
  bool HasPrefix() const { return (m_prefix && m_prefix[0]); }

  void Register(const void* ptr, size_t size, const char* symbol);
  void RegisterPC(const void* ptr, size_t size, u32 pc);
  void RegisterKey(const void* ptr, size_t size, const char* prefix, u64 key);

private:
  const char* m_prefix;
};
