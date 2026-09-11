// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/gsvector.h"
#include "common/types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
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

static s32 GaussianScalar(const std::array<s16, 4>& samples, const std::array<s16, 4>& coefficients)
{
  s32 result = static_cast<s32>(samples[0]) * static_cast<s32>(coefficients[0]);
  result += static_cast<s32>(samples[1]) * static_cast<s32>(coefficients[1]);
  result += static_cast<s32>(samples[2]) * static_cast<s32>(coefficients[2]);
  result += static_cast<s32>(samples[3]) * static_cast<s32>(coefficients[3]);
  return result >> 15;
}

static s32 GaussianSIMD(const std::array<s16, 4>& samples, const std::array<s16, 4>& coefficients)
{
  const GSVector4i sample_vector = GSVector4i::loadl<false>(samples.data());
  const GSVector4i coefficient_vector = GSVector4i::loadl<false>(coefficients.data());
  return sample_vector.madd_s16(coefficient_vector).xy().addv_s32() >> 15;
}

TEST(SPU, GaussianPhaseMajorCoefficientsMatchIndexedTable)
{
  std::array<s16, 0x200> source = {};
  for (u32 i = 0; i < source.size(); i++)
    source[i] = static_cast<s16>(i - 0x100);

  std::array<std::array<s16, 4>, 0x100> phase_major = {};
  for (u32 phase = 0; phase < phase_major.size(); phase++)
  {
    phase_major[phase] = {source[0x0FF - phase], source[0x1FF - phase], source[0x100 + phase], source[phase]};
  }

  for (u32 phase = 0; phase < phase_major.size(); phase++)
  {
    EXPECT_EQ(phase_major[phase][0], source[0x0FF - phase]);
    EXPECT_EQ(phase_major[phase][1], source[0x1FF - phase]);
    EXPECT_EQ(phase_major[phase][2], source[0x100 + phase]);
    EXPECT_EQ(phase_major[phase][3], source[phase]);
  }
}

TEST(SPU, GaussianSIMDMatchesScalar)
{
  static constexpr std::array<std::array<s16, 4>, 6> edge_samples = {{
    {{-32768, -32768, -32768, -32768}},
    {{32767, 32767, 32767, 32767}},
    {{-32768, 32767, -32768, 32767}},
    {{32767, -32768, 32767, -32768}},
    {{-32768, 0, 0, 32767}},
    {{0, 0, 0, 0}},
  }};
  static constexpr std::array<std::array<s16, 4>, 5> edge_coefficients = {{
    {{-1, 22963, 0, 0}},
    {{22963, -1, 0, 0}},
    {{8192, 8192, 8192, 8192}},
    {{-8192, -8192, -8192, -8192}},
    {{0, 0, 0, 0}},
  }};

  for (const std::array<s16, 4>& samples : edge_samples)
  {
    for (const std::array<s16, 4>& coefficients : edge_coefficients)
      EXPECT_EQ(GaussianSIMD(samples, coefficients), GaussianScalar(samples, coefficients));
  }

  std::mt19937 generator(0x47415553u);
  std::uniform_int_distribution<s32> sample_distribution(-32768, 32767);
  std::uniform_int_distribution<s32> coefficient_distribution(-22963, 22963);
  for (u32 iteration = 0; iteration < 100000; iteration++)
  {
    std::array<s16, 4> samples;
    std::array<s16, 4> coefficients;
    s64 wide_sum = 0;
    for (u32 i = 0; i < samples.size(); i++)
    {
      samples[i] = static_cast<s16>(sample_distribution(generator));
      coefficients[i] = static_cast<s16>(coefficient_distribution(generator));
      wide_sum += static_cast<s64>(samples[i]) * coefficients[i];
    }

    if (wide_sum >= std::numeric_limits<s32>::min() && wide_sum <= std::numeric_limits<s32>::max())
    {
      EXPECT_EQ(GaussianSIMD(samples, coefficients), GaussianScalar(samples, coefficients));
    }
  }
}

} // namespace
