// SPDX-FileCopyrightText: 2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC0-1.0

#pragma once

#include "types.h"

class XorShift128PlusPlus
{
public:
  struct State
  {
    u64 s0;
    u64 s1;
  };

  static constexpr State GetInitialState(u64 seed)
  {
    u64 x = seed;
    return State{SplitMix64(x), SplitMix64(x)};
  }

  constexpr XorShift128PlusPlus() : m_state(GetInitialState(0)) {}
  constexpr XorShift128PlusPlus(u64 seed) : m_state(GetInitialState(seed)) {}

  ALWAYS_INLINE const State& GetState() const { return m_state; }
  State* GetMutableStatePtr() { return &m_state; }
  void SetState(State& state) { m_state = state; }

  void Reset(u64 seed) { m_state = GetInitialState(seed); }

  u64 Next()
  {
    // https://xoroshiro.di.unimi.it/xoroshiro128plusplus.c
    u64 s0 = m_state.s0;
    u64 s1 = m_state.s1;
    u64 result = RotateLeft64(s0 + s1, 17) + s0;

    s1 ^= s0;
    m_state.s0 = RotateLeft64(s0, 49) ^ s1 ^ (s1 << 21); // a, b
    m_state.s1 = RotateLeft64(s1, 28);                   // c

    return result;
  }

  ALWAYS_INLINE u64 NextRange(u64 n)
  {
    // This constant should be folded.
    const u64 max_allowed_value = (UINT64_C(0xFFFFFFFFFFFFFFFF) / n) * n;
    for (;;)
    {
      const u64 x = Next();
      if (x > max_allowed_value)
        continue;

      return x % n;
    }
  }

  template<typename T>
  ALWAYS_INLINE T NextRange(T low, T high)
  {
    return low + static_cast<T>(NextRange(static_cast<u64>(high - low)));
  }

private:
  static constexpr u64 SplitMix64(u64& x)
  {
    // https://xoroshiro.di.unimi.it/splitmix64.c
    u64 z = (x += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }

  static ALWAYS_INLINE u64 RotateLeft64(const u64 x, int k) { return (x << k) | (x >> (64 - k)); }

  State m_state;
};
