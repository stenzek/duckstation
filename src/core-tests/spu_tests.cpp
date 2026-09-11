// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <random>

namespace {

static constexpr u32 NUM_VOICES = 24;

struct VoiceLoopResult
{
  std::array<u16, NUM_VOICES> steps;
  std::array<bool, NUM_VOICES> noise_enabled;
  s32 reverb_left;
  s32 reverb_right;

  bool operator==(const VoiceLoopResult&) const = default;
};

static VoiceLoopResult RunIndexedVoiceLoop(const std::array<s32, NUM_VOICES>& volumes,
                                           const std::array<s32, NUM_VOICES>& left,
                                           const std::array<s32, NUM_VOICES>& right,
                                           const std::array<u16, NUM_VOICES>& rates, u32 noise_modes,
                                           u32 pitch_modulation_enable, u32 reverb_on)
{
  VoiceLoopResult result = {};
  for (u32 voice = 0; voice < NUM_VOICES; voice++)
  {
    const bool noise_enabled = ((noise_modes >> voice) & 1u) != 0;
    const bool pitch_enabled = voice > 0 && ((pitch_modulation_enable >> voice) & 1u) != 0;
    u16 step = rates[voice];
    if (pitch_enabled)
    {
      const s32 factor = std::clamp(volumes[voice - 1], -0x8000, 0x7FFF) + 0x8000;
      step = static_cast<u16>(static_cast<u32>(static_cast<s32>(static_cast<s16>(step)) * factor) >> 15);
    }
    result.steps[voice] = std::min<u16>(step, 0x3FFF);

    if ((reverb_on >> voice) & 1u)
    {
      result.reverb_left += left[voice];
      result.reverb_right += right[voice];
    }

    result.noise_enabled[voice] = noise_enabled;
  }
  return result;
}

static VoiceLoopResult RunShiftedVoiceLoop(const std::array<s32, NUM_VOICES>& volumes,
                                           const std::array<s32, NUM_VOICES>& left,
                                           const std::array<s32, NUM_VOICES>& right,
                                           const std::array<u16, NUM_VOICES>& rates, u32 noise_modes,
                                           u32 pitch_modulation_enable, u32 reverb_on)
{
  VoiceLoopResult result = {};
  pitch_modulation_enable &= ~1u;
  s32 previous_voice_last_volume = 0;
  u32 voice = 0;
  for (const s32 volume : volumes)
  {
    const bool noise_enabled = (noise_modes & 1u) != 0;
    const bool pitch_enabled = (pitch_modulation_enable & 1u) != 0;
    u16 step = rates[voice];
    if (pitch_enabled)
    {
      const s32 factor = std::clamp(previous_voice_last_volume, -0x8000, 0x7FFF) + 0x8000;
      step = static_cast<u16>(static_cast<u32>(static_cast<s32>(static_cast<s16>(step)) * factor) >> 15);
    }
    result.steps[voice] = std::min<u16>(step, 0x3FFF);

    if (reverb_on & 1u)
    {
      result.reverb_left += left[voice];
      result.reverb_right += right[voice];
    }

    result.noise_enabled[voice] = noise_enabled;
    previous_voice_last_volume = volume;
    noise_modes >>= 1;
    pitch_modulation_enable >>= 1;
    reverb_on >>= 1;
    voice++;
  }
  return result;
}

TEST(SPU, ShiftedVoiceFlagsMatchIndexedFlags)
{
  std::mt19937 generator(0x4D595350u);
  std::uniform_int_distribution<u32> bits_distribution;
  std::uniform_int_distribution<s32> sample_distribution(-0x10000, 0xFFFF);
  std::uniform_int_distribution<u32> rate_distribution(0, 0xFFFF);

  for (u32 iteration = 0; iteration < 10000; iteration++)
  {
    std::array<s32, NUM_VOICES> volumes;
    std::array<s32, NUM_VOICES> left;
    std::array<s32, NUM_VOICES> right;
    std::array<u16, NUM_VOICES> rates;
    for (u32 voice = 0; voice < NUM_VOICES; voice++)
    {
      volumes[voice] = sample_distribution(generator);
      left[voice] = sample_distribution(generator);
      right[voice] = sample_distribution(generator);
      rates[voice] = static_cast<u16>(rate_distribution(generator));
    }

    const u32 noise_modes = bits_distribution(generator);
    const u32 pitch_modulation_enable = bits_distribution(generator);
    const u32 reverb_on = bits_distribution(generator);
    EXPECT_EQ(RunShiftedVoiceLoop(volumes, left, right, rates, noise_modes, pitch_modulation_enable, reverb_on),
              RunIndexedVoiceLoop(volumes, left, right, rates, noise_modes, pitch_modulation_enable, reverb_on));
  }
}

} // namespace
